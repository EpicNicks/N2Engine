#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/EditorServer.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <engine/Logger.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// The four hooks of a Lua editor extension (OnSceneOpened, OnSceneSaved, OnBeforePlay, OnAssetsChanged; E-X6) without a
// scene: the declaration (a hook is a function, needs the capability of what it reads), what a hook is given, that it is
// read-only (a write is refused before it reaches the host), its smaller budget and request cap, that an error or a
// budget trip is isolated to the extension and counts towards its fault flag, that OnBeforePlay's warnings become
// extensionOutput events every role receives, and that a host with authentication runs hooks without a Hello (as a
// viewer). Where the host calls them (after a save, an open, before a play snapshot, from an asset change) is in
// project/EditorExtensionHooksTests.cpp, which has a project and a scene.
namespace
{
    class TempProject
    {
    public:
        TempProject()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-hooks-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_root / "editor");
        }

        ~TempProject()
        {
            std::error_code error;
            fs::remove_all(_root, error);
        }

        TempProject(const TempProject &) = delete;
        TempProject &operator=(const TempProject &) = delete;

        [[nodiscard]] const fs::path &Root() const { return _root; }

        void Write(const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / "editor" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

    private:
        fs::path _root;
    };

    // An extension with these capabilities (the Lua list's inside) and these hooks (the inside of the `hooks` table)
    std::string HookExtension(const std::string &name, const std::string &capabilities, const std::string &hooks)
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { " + capabilities + " },\n"
               "  hooks = {\n" + hooks + "\n  },\n"
               "}\n";
    }

    std::vector<json> EventsOfKind(const EditorServer &server, const std::string &kind,
                                   const EventView view = EventView::Full)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0, view).events)
        {
            if (event.value("kind", "") == kind)
                found.push_back(event);
        }
        return found;
    }

    // The messages of the extensionOutput events, oldest first
    std::vector<std::string> Messages(const EditorServer &server, const EventView view = EventView::Full)
    {
        std::vector<std::string> messages;
        for (const json &event : EventsOfKind(server, "extensionOutput", view))
            messages.push_back(event.value("message", ""));
        return messages;
    }

    bool WaitUntil(const std::function<bool()> &condition, const std::chrono::milliseconds limit = 20s)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (!condition() && std::chrono::steady_clock::now() < end)
        {
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    json Find(const json &description, const std::string &id)
    {
        for (const json &extension : description.at("extensions"))
        {
            if (extension.at("id").get<std::string>() == id)
                return extension;
        }
        return json();
    }

    std::string StateOf(const json &description, const std::string &id)
    {
        const json found = Find(description, id);
        return found.is_null() ? std::string("(absent)") : found.at("state").get<std::string>();
    }

    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            {
                const std::lock_guard lock(_mutex);
                _lines.emplace_back(message);
            };
        }

        ~LogCapture() { Logger::logEvent -= _id; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] bool HasLine(const std::string_view first, const std::string_view second) const
        {
            const std::lock_guard lock(_mutex);
            for (const std::string &line : _lines)
            {
                if (line.find(first) != std::string::npos && line.find(second) != std::string::npos)
                    return true;
            }
            return false;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        std::size_t _id = 0;
    };

    class ExtensionHookTest : public ::testing::Test
    {
    protected:
        void SetUp() override { _watchdog.emplace(std::chrono::seconds(120)); }

        void TearDown() override
        {
            ExtensionHost::SetHookBudgetForTest(0, 0);
            ExtensionHost::SetMaxNestedCommandsForTest(0);
            EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; })) << "a worker never ended";
            _watchdog.reset();
        }

        std::optional<Tests::HangWatchdog> _watchdog;
    };

    const json SavedInfo = json{{"scene", "res://scenes/a.scene"}};
}

// ==================== The declaration ====================

TEST_F(ExtensionHookTest, AHookIsAFunctionAndNeedsTheCapabilityOfWhatItReads)
{
    struct Case
    {
        std::string what;
        std::string text;
        std::string error;
    };
    const std::vector<Case> cases = {
        {"an unknown hook", HookExtension("H", "\"scene.read\"", "    OnTick = function() end,"), "unknown hook 'OnTick'"},
        {"a hook that is not a function", HookExtension("H", "\"scene.read\"", "    OnSceneSaved = 1,"),
         "the hook OnSceneSaved must be a function, not a number"},
        {"OnSceneSaved without scene.read", HookExtension("H", "\"assets.read\"", "    OnSceneSaved = function() end,"),
         "the hook OnSceneSaved needs the capability scene.read"},
        {"OnSceneOpened without scene.read", HookExtension("H", "", "    OnSceneOpened = function() end,"),
         "the hook OnSceneOpened needs the capability scene.read"},
        {"OnBeforePlay without scene.read", HookExtension("H", "\"scene.write\"", "    OnBeforePlay = function() end,"),
         "the hook OnBeforePlay needs the capability scene.read"},
        {"OnAssetsChanged without assets.read", HookExtension("H", "\"scene.read\"", "    OnAssetsChanged = function() end,"),
         "the hook OnAssetsChanged needs the capability assets.read"},
        {"OnAssetsChanged with only assets.write", HookExtension("H", "\"assets.write\"", "    OnAssetsChanged = function() end,"),
         "the hook OnAssetsChanged needs the capability assets.read"},
        {"hooks as a list", HookExtension("H", "\"scene.read\"", "    function() end,"),
         "'hooks' must be a table of hook name = function, not a list"},
        {"hooks as a number",
         "return Editor.Extension { name = \"H\", apiVersion = 1, hooks = 3 }\n",
         "'hooks' must be a table of hook name = function, not a number"},
    };

    TempProject project;
    for (std::size_t i = 0; i < cases.size(); ++i)
        project.Write("case" + std::to_string(i) + ".lua", cases[i].text);
    // `hooks = {}` declares nothing and is fine, as is a file with every hook
    project.Write("z_empty.lua", HookExtension("Empty", "", ""));
    project.Write("z_good.lua", HookExtension("Good", "\"scene.read\", \"assets.read\"",
                                              "    OnSceneOpened = function() end,\n"
                                              "    OnSceneSaved = function() end,\n"
                                              "    OnBeforePlay = function() end,\n"
                                              "    OnAssetsChanged = function() end,"));

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const std::string id = "case" + std::to_string(i) + ".lua";
        EXPECT_EQ(StateOf(description, id), "failed") << cases[i].what;
        const json found = Find(description, id);
        const std::string error = found.is_null() ? std::string() : found.value("error", "");
        EXPECT_NE(error.find(cases[i].error), std::string::npos) << cases[i].what << ": " << error;
    }
    EXPECT_EQ(StateOf(description, "z_empty.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "z_good.lua"), "loaded");
}

TEST_F(ExtensionHookTest, TheHostFindsTheExtensionsOfAHookInFileOrder)
{
    TempProject project;
    project.Write("a.lua", HookExtension("A", "\"scene.read\", \"assets.read\"",
                                         "    OnSceneSaved = function() end,\n"
                                         "    OnAssetsChanged = function() end,"));
    project.Write("b.lua", HookExtension("B", "\"scene.read\"", "    OnSceneSaved = function() end,"));
    project.Write("c.lua", HookExtension("C", "", ""));

    ExtensionHost host(project.Root());
    host.Load();

    const std::vector<ExtensionHost::MenuItemRef> saved = host.FindHooks("OnSceneSaved");
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_EQ(saved[0].extensionName, "A");
    EXPECT_EQ(saved[1].extensionName, "B");
    EXPECT_TRUE(saved[0].hook);
    EXPECT_FALSE(saved[0].inspectorAction);
    EXPECT_EQ(saved[0].itemId, "OnSceneSaved");
    EXPECT_EQ(saved[0].capabilities, (std::vector<std::string>{"scene.read", "assets.read"}));
    ASSERT_EQ(host.FindHooks("OnAssetsChanged").size(), 1u);
    EXPECT_EQ(host.FindHooks("OnAssetsChanged")[0].extensionName, "A");
    EXPECT_TRUE(host.FindHooks("OnBeforePlay").empty());
    EXPECT_TRUE(host.FindHooks("OnSceneOpened").empty());
    EXPECT_TRUE(host.FindHooks("NotAHook").empty());
    EXPECT_FALSE(ExtensionHost::IsHookName("NotAHook"));
    for (const std::string_view name : ExtensionHost::HookNames)
        EXPECT_TRUE(ExtensionHost::IsHookName(name)) << name;

    // A hook is not a menu item: nothing of a hook is in the lists a client reads
    EXPECT_EQ(host.DescribeJson().at("extensions")[0].at("itemCount"), 0);
    EXPECT_TRUE(host.UiJson().at("menuItems").empty());
}

TEST_F(ExtensionHookTest, OnlyTheReadOpsAreReadOnlyOps)
{
    for (const char *op : {"log", "scene.getHierarchy", "scene.getEntity", "scene.getComponent", "scene.getLocalTransform",
                           "scene.findByName", "scene.getComponentTypes", "assets.list", "assets.readText"})
        EXPECT_TRUE(ExtensionHost::IsReadOnlyOp(op)) << op;
    for (const char *op : {"scene.createEntity", "scene.destroy", "scene.duplicate", "scene.setParent", "scene.setProperties",
                           "scene.setLocalTransform", "scene.addComponent", "scene.removeComponent", "scene.setFields",
                           "assets.writeText", "assets.createFolder", "", "shell", "scene.", "assets.readText2"})
        EXPECT_FALSE(ExtensionHost::IsReadOnlyOp(op)) << op;
}

// ==================== What a hook is given ====================

TEST_F(ExtensionHookTest, TheSceneHooksGetTheirInfoTable)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Info", "\"scene.read\"",
                                         "    OnSceneSaved = function(info) Editor.Log.Info('saved ' .. info.scene) end,\n"
                                         "    OnSceneOpened = function(info) Editor.Log.Info('opened ' .. info.scene) end,\n"
                                         "    OnBeforePlay = function(info) Editor.Log.Info('before ' .. info.scene) end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", json{{"scene", "res://a.scene"}}), 0u);
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneOpened", json{{"scene", "res://b.scene"}}), 0u);
    EXPECT_EQ(server.RunExtensionHookForTest("OnBeforePlay", json{{"scene", "res://c.scene"}}), 0u);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"saved res://a.scene", "opened res://b.scene",
                                                           "before res://c.scene"}));
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
}

TEST_F(ExtensionHookTest, OnAssetsChangedGetsTheThreeListsAsThreeArguments)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Assets", "\"assets.read\"",
                                         "    OnAssetsChanged = function(added, removed, modified)\n"
                                         "      Editor.Log.Info(#added .. ',' .. #removed .. ',' .. #modified .. ' ' .. tostring(added[1]) "
                                         ".. ' ' .. tostring(modified[1]))\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    const json info = {{"added", json::array({"res://a.txt", "res://b.txt"})},
                       {"removed", json::array()},
                       {"modified", json::array({"res://c.txt"})}};
    EXPECT_EQ(server.RunExtensionHookForTest("OnAssetsChanged", info), 0u);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"2,0,1 res://a.txt res://c.txt"}));
}

TEST_F(ExtensionHookTest, AnExtensionWithoutTheHookIsNotRunAndNothingIsLoggedForNoExtensions)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Other", "\"scene.read\"", "    OnSceneOpened = function() Editor.Log.Info('x') end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(server.RunExtensionHookForTest("NotAHook", SavedInfo), 0u);
    EXPECT_TRUE(Messages(server).empty());

    EditorServer without; // a host without the flag: there is no extension host at all
    EXPECT_EQ(without.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_TRUE(Messages(without).empty());
}

// ==================== OnBeforePlay's warnings ====================

TEST_F(ExtensionHookTest, OnBeforePlayWarningsAreExtensionOutputEventsEveryRoleGets)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Warn Play", "\"scene.read\"",
                                         "    OnBeforePlay = function(info)\n"
                                         "      Editor.Log.Info('checked ' .. info.scene)\n"
                                         "      return { warnings = { 'no camera', 'two lights' }, message = 'check the scene' }\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    EXPECT_EQ(server.RunExtensionHookForTest("OnBeforePlay", json{{"scene", "res://play.scene"}}), 3u);

    // Everything the extension wrote is in the full view; the Editor.Log line is Admin-tagged and the warnings are not
    EXPECT_EQ(Messages(server, EventView::Full),
              (std::vector<std::string>{"checked res://play.scene", "check the scene", "no camera", "two lights"}));
    const std::vector<json> publicEvents = EventsOfKind(server, "extensionOutput", EventView::PublicOnly);
    ASSERT_EQ(publicEvents.size(), 3u) << "a viewer or an editor receives the warnings and not the log line";
    for (const json &event : publicEvents)
    {
        EXPECT_EQ(event.at("extension"), "Warn Play");
        EXPECT_EQ(event.at("level"), "warn");
        EXPECT_EQ(event.at("hook"), "OnBeforePlay");
        EXPECT_TRUE(event.contains("time"));
    }
    EXPECT_EQ(publicEvents[0].at("message"), "check the scene");
    EXPECT_EQ(publicEvents[1].at("message"), "no camera");
    EXPECT_EQ(publicEvents[2].at("message"), "two lights");
}

TEST_F(ExtensionHookTest, OnBeforePlayWithNothingToSayReturnsNoWarnings)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Quiet", "\"scene.read\"",
                                         "    OnBeforePlay = function(info) return nil end,"));
    project.Write("b.lua", HookExtension("Empty", "\"scene.read\"",
                                         "    OnBeforePlay = function(info) return { warnings = {} } end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnBeforePlay", SavedInfo), 0u);
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());
}

TEST_F(ExtensionHookTest, OnBeforePlayThatReturnsMoreThanWarningsFailsAndWarnsNothing)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Greedy", "\"scene.read\"",
                                         "    OnBeforePlay = function(info) return { warnings = { 'w' }, select = {} } end,"));
    project.Write("b.lua", HookExtension("Plain", "\"scene.read\"",
                                         "    OnBeforePlay = function(info) return 'a string' end,"));
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnBeforePlay", SavedInfo), 0u) << "a result of the wrong shape is no warning";
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());
    EXPECT_TRUE(log.HasLine("hook OnBeforePlay failed", "unknown key 'select'"));
    EXPECT_TRUE(log.HasLine("hook OnBeforePlay failed", "not a result"));
}

TEST_F(ExtensionHookTest, WhatOtherHooksReturnIsIgnored)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Ignored", "\"scene.read\", \"assets.read\"",
                                         "    OnSceneSaved = function(info) return { select = 5, warnings = 3, junk = true } end,\n"
                                         "    OnSceneOpened = function(info) return 'text' end,\n"
                                         "    OnAssetsChanged = function() return 7 end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    const json assets = {{"added", json::array()}, {"removed", json::array()}, {"modified", json::array()}};
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
        EXPECT_EQ(server.RunExtensionHookForTest("OnSceneOpened", SavedInfo), 0u);
        EXPECT_EQ(server.RunExtensionHookForTest("OnAssetsChanged", assets), 0u);
    }
    // Fifteen runs: not one counted as a failure (three would have faulted it)
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());
}

// ==================== Read-only ====================

TEST_F(ExtensionHookTest, AHookCannotChangeTheSceneOrTheAssets)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Writer", "\"scene.read\", \"scene.write\", \"assets.read\", \"assets.write\"",
                                         "    OnSceneSaved = function(info)\n"
                                         "      local ok, message = pcall(function() return Editor.Scene.CreateEntity { name = 'x' } end)\n"
                                         "      Editor.Log.Info(tostring(ok) .. ' ' .. tostring(message))\n"
                                         "      local ok2, message2 = pcall(Editor.Assets.WriteText, 'res://x.txt', 'x')\n"
                                         "      Editor.Log.Info(tostring(ok2) .. ' ' .. tostring(message2))\n"
                                         "      local ok3, message3 = pcall(Editor.Assets.CreateFolder, 'res://folder')\n"
                                         "      Editor.Log.Info(tostring(ok3) .. ' ' .. tostring(message3))\n"
                                         "      local ok4 = pcall(Editor.Scene.GetComponentTypes)\n"
                                         "      Editor.Log.Info('read ' .. tostring(ok4))\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    const uint32_t revision = server.GetSceneRevision();

    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    const std::vector<std::string> messages = Messages(server);
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[0].rfind("false ", 0), 0u) << messages[0];
    EXPECT_NE(messages[0].find("a hook is read-only: it cannot change the scene or the assets"), std::string::npos) << messages[0];
    EXPECT_EQ(messages[1].rfind("false ", 0), 0u) << messages[1];
    EXPECT_NE(messages[1].find("a hook is read-only"), std::string::npos) << messages[1];
    EXPECT_EQ(messages[2].rfind("false ", 0), 0u) << messages[2];
    EXPECT_NE(messages[2].find("a hook is read-only"), std::string::npos) << messages[2];
    EXPECT_EQ(messages[3], "read true") << "a read still works";
    EXPECT_EQ(server.GetSceneRevision(), revision);
    // Refused calls the script caught are no failure of the hook
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
}

TEST_F(ExtensionHookTest, AHookThatDoesNotCatchTheRefusalFailsAndCountsAgainstTheExtension)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Uncaught", "\"scene.read\", \"scene.write\"",
                                         "    OnSceneSaved = function(info) Editor.Scene.CreateEntity { name = 'x' } end,"));
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u) << "the host never throws for a hook";
    EXPECT_TRUE(log.HasLine("hook OnSceneSaved failed", "a hook is read-only"));
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded) << "one failure";
}

TEST_F(ExtensionHookTest, AHookHasNoFunctionOfACapabilityItDidNotDeclare)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Narrow", "\"scene.read\"",
                                         "    OnSceneSaved = function(info)\n"
                                         "      Editor.Log.Info(type(Editor.Scene.GetHierarchy) .. ' ' .. type(Editor.Scene.CreateEntity) .. ' ' "
                                         ".. type(Editor.Assets))\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"function nil nil"}));
}

TEST_F(ExtensionHookTest, AHookIsCappedAtAThousandRequests)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Chatty", "\"scene.read\"",
                                         "    OnSceneSaved = function(info)\n"
                                         "      for i = 1, 2000 do\n"
                                         "        local ok = pcall(Editor.Scene.GetComponentTypes)\n"
                                         "        if not ok then Editor.Log.Info('stopped at ' .. i) return end\n"
                                         "      end\n"
                                         "      Editor.Log.Info('never stopped')\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"stopped at " + std::to_string(ExtensionHost::MaxHookNestedCommands + 1)}));
}

// ==================== Isolation, the fault flag, the budget ====================

TEST_F(ExtensionHookTest, AnErrorInOneExtensionsHookIsIsolatedAndCountsTowardsItsFaultFlag)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Broken", "\"scene.read\"",
                                         "    OnSceneSaved = function(info) error('boom') end,"));
    project.Write("b.lua", HookExtension("Fine", "\"scene.read\"",
                                         "    OnSceneSaved = function(info) Editor.Log.Info('b ran') end,"));
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    ExtensionHost &host = *server.GetEditorExtensions();
    const uint32_t revision = host.UiRevision();

    for (int run = 1; run <= 3; ++run)
        EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u) << run;
    EXPECT_EQ(Messages(server).size(), 3u) << "the other extension's hook ran every time";
    EXPECT_TRUE(log.HasLine("Editor extension Broken@", "hook OnSceneSaved failed"));
    EXPECT_TRUE(log.HasLine("hook OnSceneSaved failed", "boom"));

    // Three failures in ten minutes: faulted until a reload; the other is untouched
    EXPECT_EQ(host.StateOf("a.lua"), ExtensionState::Faulted);
    EXPECT_EQ(host.StateOf("b.lua"), ExtensionState::Loaded);
    EXPECT_GT(host.UiRevision(), revision) << "the fault is a UI change (extensionsChanged)";
    EXPECT_FALSE(EventsOfKind(server, "extensionsChanged").empty());

    // A faulted extension's hooks are not called
    EXPECT_EQ(host.FindHooks("OnSceneSaved").size(), 1u);
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(Messages(server).size(), 4u);
}

TEST_F(ExtensionHookTest, ABudgetTripInAHookFaultsTheExtensionAtOnceAndTheOthersStillRun)
{
    ExtensionHost::SetHookBudgetForTest(1'000'000, 2000);
    TempProject project;
    project.Write("a.lua", HookExtension("Spinner", "\"scene.read\"",
                                         "    OnSceneSaved = function(info) while true do end end,"));
    project.Write("b.lua", HookExtension("Fine", "\"scene.read\"",
                                         "    OnSceneSaved = function(info) Editor.Log.Info('b ran') end,"));
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Faulted) << "one trip is enough";
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("b.lua"), ExtensionState::Loaded);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"b ran"}));
    EXPECT_TRUE(log.HasLine("hook OnSceneSaved failed", "disabled until reload"));
}

TEST_F(ExtensionHookTest, TheHookBudgetIsSmallerThanTheInvocationBudget)
{
    // The values of design 6.4: 20 million instructions and half a second, against 200 million and five seconds
    EXPECT_EQ(Scripting::SandboxDefaults::HookBudget.instructions, 20'000'000u);
    EXPECT_EQ(Scripting::SandboxDefaults::HookBudget.maxMillis, 500u);
    EXPECT_LT(Scripting::SandboxDefaults::HookBudget.instructions, Scripting::SandboxDefaults::InvocationBudget.instructions);
    EXPECT_LT(Scripting::SandboxDefaults::HookBudget.maxMillis, Scripting::SandboxDefaults::InvocationBudget.maxMillis);
    EXPECT_LT(ExtensionHost::MaxHookNestedCommands, ExtensionHost::MaxNestedCommands);
}

// ==================== Who a hook runs as ====================

TEST_F(ExtensionHookTest, AHostWithAuthenticationRunsAHookWithoutAHelloAsAViewer)
{
    TempProject project;
    project.Write("a.lua", HookExtension("Reader", "\"scene.read\"",
                                         "    OnSceneSaved = function(info)\n"
                                         "      local types = Editor.Scene.GetComponentTypes()\n"
                                         "      Editor.Log.Info('types ' .. tostring(#types > 0))\n"
                                         "    end,"));
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("a-token-the-test-never-sends"));
    ASSERT_TRUE(server.RequiresAccessToken());
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    // No client said Hello (a poll or a save has none that matters): the hook's reads run as a viewer, which the role
    // table lets read
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", SavedInfo), 0u);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"types true"}));
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
}
