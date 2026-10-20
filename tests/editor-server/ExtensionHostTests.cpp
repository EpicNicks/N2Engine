#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <editor-server/ExtensionHost.hpp>
#include <engine/scripting/LuaSandbox.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// The ExtensionHost (Lua editor extensions, PR E-X2) without a socket: discovery of <project>/editor/**/*.lua, one
// sandbox state per file, the shape checks of Editor.Extension{...}, the registry, the fault flag, the worker with its
// abandon deadline, and the proof that the readers run no Lua. EditorExtensionCommandsTests.cpp covers the same host
// through the protocol (the commands, the flag-off answers, the roles).
namespace
{
    class TempProject
    {
    public:
        TempProject()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-host-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_root / "editor");
            fs::create_directories(_root / "assets");
        }

        ~TempProject()
        {
            std::error_code error;
            fs::remove_all(_root, error);
        }

        TempProject(const TempProject &) = delete;
        TempProject &operator=(const TempProject &) = delete;

        [[nodiscard]] const fs::path &Root() const { return _root; }

        void Write(const std::string &relative, const std::string &text) const { WriteIn("editor", relative, text); }
        void WriteAsset(const std::string &relative, const std::string &text) const { WriteIn("assets", relative, text); }

        void Remove(const std::string &relative) const
        {
            std::error_code error;
            fs::remove(_root / "editor" / relative, error);
        }

    private:
        void WriteIn(const char *folder, const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / folder / relative;
            fs::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

        fs::path _root;
    };

    // A file that declares one extension with one item (the id and name are given so files can differ)
    std::string Valid(const std::string &name, const std::string &itemId = "align.snap")
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { \"scene.read\", \"scene.write\" },\n"
               "  menu = {\n"
               "    {\n"
               "      id = \"" + itemId + "\",\n"
               "      path = \"Tools/Align/Snap selection to grid\",\n"
               "      enabledWhen = { selection = \"nonEmpty\" },\n"
               "      params = {\n"
               "        grid = { type = \"number\", default = 1.0, min = 0.01, max = 100, tooltip = \"Grid size in metres\" },\n"
               "        snapY = { type = \"bool\", default = false },\n"
               "      },\n"
               "      run = function(ctx, p) end,\n"
               "    },\n"
               "  },\n"
               "}\n";
    }

    // A declaration with the given pieces, for the shape tests: Editor.Extension { name, apiVersion, ... extra }
    std::string Declaration(const std::string &extra, const std::string &name = "\"Shape\"",
                            const std::string &apiVersion = "1")
    {
        std::string text = "return Editor.Extension {\n";
        if (!name.empty())
            text += "  name = " + name + ",\n";
        if (!apiVersion.empty())
            text += "  apiVersion = " + apiVersion + ",\n";
        text += extra + "\n}\n";
        return text;
    }

    // A menu with one item that has a valid id, run and (unless given) path, plus the extra keys of `itemBody`
    std::string OneItem(const std::string &itemBody, const std::string &path = "Tools/Item")
    {
        return "  menu = { { id = \"item\", path = \"" + path + "\", run = function() end" +
               (itemBody.empty() ? std::string() : ", " + itemBody) + " } },";
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

    std::string ErrorOf(const json &description, const std::string &id)
    {
        const json found = Find(description, id);
        return found.is_null() ? std::string() : found.value("error", "");
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

    // What a test thread opens to let a worker that a hook holds go on; shared with the hook, so a worker that is still
    // waiting never touches a destroyed object
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;

        void Wait()
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return open; });
        }

        void Open()
        {
            {
                const std::lock_guard lock(mutex);
                open = true;
            }
            changed.notify_all();
        }
    };

    std::atomic<long> g_instructions{0};

    void CountInstructions(lua_State *, lua_Debug *)
    {
        ++g_instructions;
    }

    class ExtensionHostTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog.emplace();
        }

        void TearDown() override
        {
            ExtensionHost::SetWorkerHookForTest({});
            ExtensionHost::SetAbandonMillisForTest(0);
            if (_gate)
            {
                _gate->Open();
            }
            EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; })) << "a worker never ended";
            _watchdog.reset();
        }

        // A hook that holds every worker whose file id starts with `prefix` until the gate opens
        void HoldWorkers(const std::string &prefix)
        {
            _gate = std::make_shared<Gate>();
            const std::shared_ptr<Gate> gate = _gate;
            ExtensionHost::SetWorkerHookForTest([gate, prefix](const std::string &id)
            {
                if (id.starts_with(prefix))
                {
                    gate->Wait();
                }
            });
        }

        std::shared_ptr<Gate> _gate;
        std::optional<Tests::HangWatchdog> _watchdog;
    };
}

// ==================== SHA-256 ====================

TEST(ExtensionHostSha256Test, MatchesTheKnownVectors)
{
    EXPECT_EQ(ExtensionHost::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(ExtensionHost::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding needs a second block
    EXPECT_EQ(ExtensionHost::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(ExtensionHost::Sha256Hex(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Exactly one block of data, and the lengths around the padding boundary, all distinct 64-digit lowercase hex
    std::vector<std::string> seen;
    for (const std::size_t size : {54u, 55u, 56u, 63u, 64u, 65u, 119u, 120u, 128u})
    {
        const std::string digest = ExtensionHost::Sha256Hex(std::string(size, 'x'));
        EXPECT_EQ(digest.size(), 64u) << size;
        EXPECT_EQ(std::count(seen.begin(), seen.end(), digest), 0) << size;
        seen.push_back(digest);
    }
}

// ==================== Discovery ====================

TEST_F(ExtensionHostTest, AProjectWithoutAnEditorFolderHasNoExtensions)
{
    TempProject project;
    std::error_code error;
    fs::remove_all(project.Root() / "editor", error);
    ExtensionHost host(project.Root());
    host.Load();
    EXPECT_EQ(host.ExtensionCount(), 0u);
    EXPECT_EQ(host.UiRevision(), 1u);
    EXPECT_TRUE(host.DescribeJson().at("enabled").get<bool>());

    ExtensionHost noProject{fs::path{}};
    noProject.Load();
    EXPECT_EQ(noProject.ExtensionCount(), 0u);
}

TEST_F(ExtensionHostTest, FindsLuaFilesBelowTheEditorFolderInSortedOrder)
{
    TempProject project;
    project.Write("b.lua", Valid("B", "b.item"));
    project.Write("a.lua", Valid("A", "a.item"));
    project.Write("tools/deep/c.lua", Valid("C", "c.item"));
    project.Write("Upper.LUA", Valid("Upper", "upper.item"));
    project.Write("notes.txt", "not an extension");
    project.Write("tools/readme.md", "not an extension");

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    std::vector<std::string> ids;
    for (const json &extension : description.at("extensions"))
        ids.push_back(extension.at("id").get<std::string>());
    EXPECT_EQ(ids, (std::vector<std::string>{"Upper.LUA", "a.lua", "b.lua", "tools/deep/c.lua"}))
        << "byte order of the id, '/' separators, any case of the extension, nothing but .lua";
    for (const std::string &id : ids)
        EXPECT_EQ(StateOf(description, id), "loaded") << id;
}

TEST_F(ExtensionHostTest, AFileOutsideTheEditorFolderIsNeverLoaded)
{
    TempProject project;
    // The same text under assets/ (where WriteTextAsset can put a file) and beside the project: not extensions
    project.WriteAsset("Evil.lua", Valid("Evil", "evil.item"));
    project.WriteAsset("editor/Evil2.lua", Valid("Evil2", "evil2.item"));
    {
        std::ofstream(project.Root() / "Top.lua", std::ios::binary) << Valid("Top", "top.item");
    }
    project.Write("Real.lua", Valid("Real", "real.item"));

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    ASSERT_EQ(description.at("extensions").size(), 1u);
    EXPECT_EQ(description.at("extensions")[0].at("id"), "Real.lua");
}

TEST_F(ExtensionHostTest, AtMost64FilesLoadTheRestAreLeftOut)
{
    TempProject project;
    for (int i = 0; i < 66; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "e%03d.lua", i);
        project.Write(name, Valid("Ext" + std::to_string(i), "item" + std::to_string(i)));
    }
    ExtensionHost host(project.Root());
    host.Load();
    EXPECT_EQ(host.ExtensionCount(), ExtensionHost::MaxFiles);
    EXPECT_EQ(host.LoadedCount(), ExtensionHost::MaxFiles);
    const json description = host.DescribeJson();
    EXPECT_FALSE(Find(description, "e063.lua").is_null());
    EXPECT_TRUE(Find(description, "e064.lua").is_null()) << "sorted order: the first 64 are the ones that load";
}

TEST_F(ExtensionHostTest, ALargeFileFailsWithoutBeingLoaded)
{
    TempProject project;
    project.Write("big.lua", "-- " + std::string(ExtensionHost::MaxFileBytes, 'x') + "\n" + Valid("Big"));
    project.Write("ok.lua", Valid("Ok", "ok.item"));
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "big.lua"), "failed");
    EXPECT_NE(ErrorOf(description, "big.lua").find("larger than"), std::string::npos) << ErrorOf(description, "big.lua");
    EXPECT_TRUE(Find(description, "big.lua").at("sha256").get<std::string>().empty()) << "a file that is not read has no hash";
    EXPECT_EQ(StateOf(description, "ok.lua"), "loaded");
}

TEST_F(ExtensionHostTest, SymbolicLinksAreNotFollowed)
{
    TempProject project;
    TempProject outside;
    outside.Write("Outside.lua", Valid("Outside", "outside.item"));
    project.Write("Real.lua", Valid("Real", "real.item"));

    std::error_code error;
    fs::create_symlink(outside.Root() / "editor" / "Outside.lua", project.Root() / "editor" / "Link.lua", error);
    if (error)
    {
        GTEST_SKIP() << "this system does not let the test make a symbolic link: " << error.message();
    }
    fs::create_directory_symlink(outside.Root() / "editor", project.Root() / "editor" / "linked", error);

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    ASSERT_EQ(description.at("extensions").size(), 1u) << description.dump();
    EXPECT_EQ(description.at("extensions")[0].at("id"), "Real.lua");
}

TEST_F(ExtensionHostTest, AnEditorFolderThatIsALinkIsNotSearched)
{
    TempProject project;
    TempProject elsewhere;
    elsewhere.Write("Elsewhere.lua", Valid("Elsewhere", "elsewhere.item"));
    std::error_code error;
    fs::remove_all(project.Root() / "editor", error);
    fs::create_directory_symlink(elsewhere.Root() / "editor", project.Root() / "editor", error);
    if (error)
    {
        GTEST_SKIP() << "this system does not let the test make a symbolic link: " << error.message();
    }
    ExtensionHost host(project.Root());
    host.Load();
    EXPECT_EQ(host.ExtensionCount(), 0u);
}

// ==================== What a loaded file is ====================

TEST_F(ExtensionHostTest, ALoadedFileIsInTheRegistryWithItsDeclaration)
{
    TempProject project;
    const std::string source = Valid("Align Tools");
    project.Write("AlignTools.lua", source);
    ExtensionHost host(project.Root());
    host.Load();

    const json description = host.DescribeJson();
    EXPECT_TRUE(description.at("enabled").get<bool>());
    EXPECT_EQ(description.at("uiRevision"), 1);
    const json extension = Find(description, "AlignTools.lua");
    ASSERT_FALSE(extension.is_null());
    EXPECT_EQ(extension.at("name"), "Align Tools");
    EXPECT_EQ(extension.at("state"), "loaded");
    EXPECT_EQ(extension.at("apiVersion"), 1);
    EXPECT_EQ(extension.at("sha256").get<std::string>(), ExtensionHost::Sha256Hex(source));
    EXPECT_EQ(extension.at("capabilities"), (json::array({"scene.read", "scene.write"})));
    EXPECT_EQ(extension.at("itemCount"), 1);
    EXPECT_FALSE(extension.contains("error"));

    const json ui = host.UiJson();
    EXPECT_EQ(ui.at("uiRevision"), 1);
    EXPECT_TRUE(ui.at("inspectorSections").is_array());
    EXPECT_TRUE(ui.at("inspectorSections").empty());
    ASSERT_EQ(ui.at("menuItems").size(), 1u);
    const json item = ui.at("menuItems")[0];
    EXPECT_EQ(item.at("id"), "align.snap");
    EXPECT_EQ(item.at("extension"), "AlignTools.lua");
    EXPECT_EQ(item.at("path"), "Tools/Align/Snap selection to grid");
    EXPECT_EQ(item.at("title"), "Snap selection to grid") << "the last segment of the path unless the file gave a title";
    EXPECT_EQ(item.at("enabledWhen"), (json{{"selection", "nonEmpty"}}));
    EXPECT_EQ(item.at("minRole"), "editor");
    EXPECT_FALSE(item.contains("confirm"));

    ASSERT_TRUE(item.contains("form"));
    const json fields = item.at("form").at("fields");
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].at("name"), "grid") << "fields are sorted by name";
    EXPECT_EQ(fields[0].at("kind"), "Float");
    EXPECT_EQ(fields[0].at("typeName"), "float");
    EXPECT_EQ(fields[0].at("min"), 0.01);
    EXPECT_EQ(fields[0].at("max"), 100);
    EXPECT_EQ(fields[0].at("tooltip"), "Grid size in metres");
    EXPECT_EQ(fields[0].at("displayName"), "Grid");
    EXPECT_FALSE(fields[0].at("hidden").get<bool>());
    EXPECT_FALSE(fields[0].at("readOnly").get<bool>());
    EXPECT_EQ(fields[1].at("name"), "snapY");
    EXPECT_EQ(fields[1].at("kind"), "Bool");
    EXPECT_EQ(item.at("form").at("defaults").at("grid"), 1.0);
    EXPECT_EQ(item.at("form").at("defaults").at("snapY"), false);
}

TEST_F(ExtensionHostTest, ItemsKeepTheirOptionsAndFormsTheirKinds)
{
    TempProject project;
    project.Write("Kinds.lua", R"LUA(
return Editor.Extension {
  name = "Kinds",
  apiVersion = 1,
  menu = {
    {
      id = "kinds.all",
      path = "Tools/Kinds",
      title = "Every kind",
      minRole = "admin",
      confirm = "Really?",
      enabledWhen = { selection = "one", componentType = "Light" },
      params = {
        count = { type = "int", default = 3, min = 1, max = 10 },
        label = { type = "string", default = "hi" },
        offset = { type = "Vector3", default = { x = 1, y = 2, z = 3 } },
        target = { type = "GameObject" },
        mode = { type = "enum", options = { "Fast", "Slow" }, default = "Slow", displayName = "Speed mode" },
      },
      run = function() end,
    },
  },
}
)LUA");
    ExtensionHost host(project.Root());
    host.Load();
    ASSERT_EQ(host.LoadedCount(), 1u) << host.DescribeJson().dump();
    const json item = host.UiJson().at("menuItems")[0];
    EXPECT_EQ(item.at("title"), "Every kind");
    EXPECT_EQ(item.at("minRole"), "admin");
    EXPECT_EQ(item.at("confirm"), "Really?");
    EXPECT_EQ(item.at("enabledWhen"), (json{{"selection", "one"}, {"componentType", "Light"}}));

    std::map<std::string, json> byName;
    for (const json &field : item.at("form").at("fields"))
        byName[field.at("name").get<std::string>()] = field;
    ASSERT_EQ(byName.size(), 5u);
    EXPECT_EQ(byName["count"].at("kind"), "Int");
    EXPECT_EQ(byName["label"].at("kind"), "String");
    EXPECT_EQ(byName["offset"].at("kind"), "Vector3");
    EXPECT_EQ(byName["target"].at("kind"), "GameObjectRef");
    EXPECT_EQ(byName["mode"].at("kind"), "Enum");
    EXPECT_EQ(byName["mode"].at("enumOptions"), (json::array({"Fast", "Slow"})));
    EXPECT_EQ(byName["mode"].at("displayName"), "Speed mode");

    const json defaults = item.at("form").at("defaults");
    EXPECT_EQ(defaults.at("count"), 3);
    EXPECT_EQ(defaults.at("label"), "hi");
    EXPECT_EQ(defaults.at("offset"), (json{{"x", 1}, {"y", 2}, {"z", 3}}));
    EXPECT_TRUE(defaults.at("target").is_null());
    EXPECT_EQ(defaults.at("mode"), "Slow");
}

TEST_F(ExtensionHostTest, AFileWithNoMenuOrCapabilitiesLoadsWithNothing)
{
    TempProject project;
    project.Write("Empty.lua", "return Editor.Extension { name = \"Empty\", apiVersion = 1 }");
    project.Write("EmptyLists.lua", "return Editor.Extension { name = \"Lists\", apiVersion = 1, capabilities = {}, menu = {} }");
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "Empty.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "EmptyLists.lua"), "loaded");
    EXPECT_EQ(Find(description, "Empty.lua").at("itemCount"), 0);
    EXPECT_TRUE(Find(description, "Empty.lua").at("capabilities").empty());
}

TEST_F(ExtensionHostTest, AByteOrderMarkIsNotPartOfTheScript)
{
    TempProject project;
    project.Write("Bom.lua", "\xEF\xBB\xBF" + Valid("Bom", "bom.item"));
    ExtensionHost host(project.Root());
    host.Load();
    EXPECT_EQ(StateOf(host.DescribeJson(), "Bom.lua"), "loaded") << ErrorOf(host.DescribeJson(), "Bom.lua");
}

TEST_F(ExtensionHostTest, APrecompiledChunkIsRefused)
{
    TempProject project;
    project.Write("Binary.lua", std::string("\x1b") + "Lua\x54\x00 not a real chunk");
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "Binary.lua"), "failed");
    EXPECT_NE(ErrorOf(description, "Binary.lua").find("binary chunk"), std::string::npos) << ErrorOf(description, "Binary.lua");
}

// ==================== A broken file stops nobody ====================

TEST_F(ExtensionHostTest, ABrokenFileDoesNotStopTheOthers)
{
    TempProject project;
    project.Write("a_ok.lua", Valid("A", "a.item"));
    project.Write("b_syntax.lua", "return Editor.Extension {");
    project.Write("c_runtime.lua", "error('boom')");
    project.Write("d_nothing.lua", "return {}");
    project.Write("e_undefined.lua", "return Editor.Missing()");
    project.Write("f_ok.lua", Valid("F", "f.item"));

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a_ok.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "b_syntax.lua"), "failed");
    EXPECT_EQ(StateOf(description, "c_runtime.lua"), "failed");
    EXPECT_EQ(StateOf(description, "d_nothing.lua"), "failed");
    EXPECT_EQ(StateOf(description, "e_undefined.lua"), "failed");
    EXPECT_EQ(StateOf(description, "f_ok.lua"), "loaded");
    EXPECT_EQ(host.LoadedCount(), 2u);
    for (const char *broken : {"b_syntax.lua", "c_runtime.lua", "d_nothing.lua", "e_undefined.lua"})
        EXPECT_FALSE(ErrorOf(description, broken).empty()) << broken;
    EXPECT_NE(ErrorOf(description, "c_runtime.lua").find("c_runtime.lua:1: boom"), std::string::npos)
        << ErrorOf(description, "c_runtime.lua");

    // The items of the two that loaded are listed; the broken ones have none
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);
    // A failed file keeps its id as its name (it never declared one) and has no declaration
    EXPECT_EQ(Find(description, "b_syntax.lua").at("name"), "b_syntax.lua");
    EXPECT_EQ(Find(description, "b_syntax.lua").at("apiVersion"), 0);
    EXPECT_EQ(Find(description, "b_syntax.lua").at("itemCount"), 0);
}

TEST_F(ExtensionHostTest, ErrorTextNeverShowsAHostPath)
{
    TempProject project;
    project.Write("sub/Raises.lua", "local t = nil\nreturn t.field");
    project.Write("Syntax.lua", "return = nope");
    project.Write("Declares.lua", Declaration("  capabilities = { \"scene.nuke\" },"));
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();

    const std::string root = project.Root().string();
    const std::string generic = project.Root().generic_string();
    const std::string parent = project.Root().parent_path().string();
    for (const json &extension : description.at("extensions"))
    {
        const std::string error = extension.value("error", "");
        EXPECT_FALSE(error.empty()) << extension.at("id");
        EXPECT_EQ(error.find(root), std::string::npos) << error;
        EXPECT_EQ(error.find(generic), std::string::npos) << error;
        EXPECT_EQ(error.find(parent), std::string::npos) << error;
    }
    EXPECT_NE(ErrorOf(description, "sub/Raises.lua").find("sub/Raises.lua:2:"), std::string::npos)
        << "the chunk name is the file's path inside editor/: " << ErrorOf(description, "sub/Raises.lua");
    // Nothing of the listing as a whole shows it either (JSON text doubles a backslash)
    std::string escaped;
    for (const char c : parent)
    {
        escaped += c;
        if (c == '\\')
            escaped += c;
    }
    for (const std::string &text : {description.dump(), host.UiJson().dump()})
    {
        EXPECT_EQ(text.find(parent), std::string::npos);
        EXPECT_EQ(text.find(escaped), std::string::npos);
    }
}

// ==================== The shape of Editor.Extension{...} ====================

namespace
{
    struct ShapeCase
    {
        const char *what;
        std::string source;
        const char *expectedInError;
    };

    std::vector<ShapeCase> ShapeCases()
    {
        std::vector<ShapeCase> cases;
        const auto add = [&](const char *what, std::string source, const char *expected)
        {
            cases.push_back({what, std::move(source), expected});
        };

        add("an unknown capability", Declaration("  capabilities = { \"scene.nuke\" },"), "unknown capability");
        add("a capability of the reserved set", Declaration("  capabilities = { \"project.write\" },"), "unknown capability");
        add("a capability listed twice", Declaration("  capabilities = { \"scene.read\", \"scene.read\" },"), "listed twice");
        add("a capability that is not a string", Declaration("  capabilities = { 5 },"), "must be strings");
        add("capabilities that are not a list", Declaration("  capabilities = \"scene.read\","), "must be a list of strings");
        add("no name", Declaration("", ""), "'name' is required");
        add("a name that is not a string", Declaration("", "5"), "'name' must be a string");
        add("a function as the name", Declaration("", "function() end"), "'name' must be a string, not a function");
        add("a name with a parent directory", Declaration("", "\"../evil\""), "path characters");
        add("a name that is a parent directory", Declaration("", "\"..\""), "path characters");
        add("a name with a slash", Declaration("", "\"a/b\""), "path characters");
        add("a name with a backslash", Declaration("", "\"a\\\\b\""), "path characters");
        add("a name with a drive", Declaration("", "\"C:evil\""), "path characters");
        add("a name that starts with a dot", Declaration("", "\".hidden\""), "path characters");
        add("an empty name", Declaration("", "\"\""), "'name' must be 1 to 64 bytes");
        add("a name with a control character", Declaration("", "\"bad\\nname\""), "control characters");
        add("a name that is too long", Declaration("", "string.rep(\"x\", 65)"), "'name' must be 1 to 64 bytes");
        add("no apiVersion", Declaration("", "\"Shape\"", ""), "'apiVersion' is required");
        add("an apiVersion this host does not speak", Declaration("", "\"Shape\"", "2"), "apiVersion 2 is not supported");
        add("an apiVersion that is a string", Declaration("", "\"Shape\"", "\"1\""), "'apiVersion' must be an integer");
        add("an apiVersion with a fraction", Declaration("", "\"Shape\"", "1.5"), "'apiVersion' must be an integer");
        add("a key this API version does not know", Declaration("  hooks = {},"), "unknown key 'hooks'");
        add("a menu that is not a list", Declaration("  menu = 5,"), "'menu' must be a list");
        add("a menu that mixes a list and names", Declaration("  menu = { { id = \"a\", path = \"T/a\", run = function() end }, extra = 1 },"),
            "mixes a list and named entries");
        add("a menu list with a gap", Declaration("  menu = { [1] = { id = \"a\", path = \"T/a\", run = function() end }, [3] = {} },"),
            "has a gap");
        add("65 menu items",
            Declaration("  menu = (function() local menu = {} for i = 1, 65 do menu[i] = { id = \"i\" .. i, path = \"Tools/x\" .. i, run = function() end } end return menu end)(),"),
            "more than 64 items");
        add("two items with one id",
            Declaration("  menu = { { id = \"same\", path = \"T/a\", run = function() end }, { id = \"same\", path = \"T/b\", run = function() end } },"),
            "used by two items");
        add("an item id with a space", Declaration("  menu = { { id = \"bad id\", path = \"T/a\", run = function() end } },"),
            "must be letters, digits");
        add("an item id that starts with a dot", Declaration("  menu = { { id = \".a\", path = \"T/a\", run = function() end } },"),
            "must be letters, digits");
        add("an item with no id", Declaration("  menu = { { path = \"T/a\", run = function() end } },"), "'id' is required");
        add("an item with no path", Declaration("  menu = { { id = \"a\", run = function() end } },"), "'path' is required");
        add("an item path with a parent segment", Declaration(OneItem("", "Tools/../x")),
            "segments separated");
        add("an item path with an empty segment", Declaration(OneItem("", "Tools//x")),
            "segments separated");
        add("an item with no run", Declaration("  menu = { { id = \"a\", path = \"T/a\" } },"), "'run' must be a function");
        add("an item whose run is a string", Declaration("  menu = { { id = \"a\", path = \"T/a\", run = \"x\" } },"),
            "'run' must be a function");
        add("an item key this API version does not know", Declaration(OneItem("typo = 1")), "unknown key 'typo'");
        add("a minRole that is not editor or admin", Declaration(OneItem("minRole = \"viewer\"")), "'minRole' must be");
        add("an enabledWhen with a wrong selection", Declaration(OneItem("enabledWhen = { selection = \"many\" }")),
            "enabledWhen.selection");
        add("an enabledWhen with an unknown key", Declaration(OneItem("enabledWhen = { when = 1 }")), "unknown key 'when'");
        add("a confirm that is not text", Declaration(OneItem("confirm = 5")), "'confirm' must be a string");
        add("params that are not a table", Declaration(OneItem("params = 5")), "'params' must be a table");
        add("a param of an unknown type", Declaration(OneItem("params = { x = { type = \"Matrix\" } }")), "is not one of number");
        add("a param that is not a table", Declaration(OneItem("params = { x = 5 }")), "a param must be a table");
        add("a param with an unknown key", Declaration(OneItem("params = { x = { type = \"int\", step = 2 } }")),
            "unknown key 'step'");
        add("a param name that is not an identifier", Declaration(OneItem("params = { [\"a b\"] = { type = \"int\" } }")),
            "the param name");
        add("a param default of the wrong type", Declaration(OneItem("params = { x = { type = \"number\", default = \"1\" } }")),
            "'default' must be a number");
        add("an int default with a fraction", Declaration(OneItem("params = { x = { type = \"int\", default = 1.5 } }")),
            "whole number");
        add("a default outside the range",
            Declaration(OneItem("params = { x = { type = \"number\", min = 1, max = 2, default = 5 } }")),
            "outside 'min' and 'max'");
        add("a min above the max", Declaration(OneItem("params = { x = { type = \"number\", min = 3, max = 2 } }")),
            "'min' is greater than 'max'");
        add("a range on a string", Declaration(OneItem("params = { x = { type = \"string\", min = 1 } }")), "'min' and 'max' only go");
        add("an enum with no options", Declaration(OneItem("params = { x = { type = \"enum\" } }")), "needs 'options'");
        add("an enum that lists an option twice",
            Declaration(OneItem("params = { x = { type = \"enum\", options = { \"a\", \"a\" } } }")), "listed twice");
        add("an enum default that is not an option",
            Declaration(OneItem("params = { x = { type = \"enum\", options = { \"a\" }, default = \"b\" } }")),
            "must be one of its 'options'");
        add("options on a number", Declaration(OneItem("params = { x = { type = \"number\", options = { \"a\" } } }")),
            "only goes with type = \"enum\"");
        add("a Vector3 default that is not a vector", Declaration(OneItem("params = { x = { type = \"Vector3\", default = 1 } }")),
            "'default' of a Vector3");
        add("a GameObject default", Declaration(OneItem("params = { x = { type = \"GameObject\", default = \"id\" } }")),
            "has no 'default'");
        add("an oversized form",
            Declaration(OneItem("params = (function() local params = {} for i = 1, 33 do params[\"f\" .. i] = { type = \"int\" } end return params end)()")),
            "more than 32 fields");
        add("a string past the cap",
            Declaration(OneItem("confirm = string.rep(\"x\", 5000)")), "longer than 4096 bytes");
        add("a table that contains itself", "local t = { name = \"Cycle\", apiVersion = 1 }\nt.self = t\nreturn Editor.Extension(t)\n",
            "nested deeper than 8 levels");
        add("a key that is neither a string nor an integer", Declaration("  [true] = 1,"), "neither a string nor an integer");
        add("no call of Editor.Extension", "return {}", "did not call Editor.Extension");
        add("Editor.Extension given a number", "return Editor.Extension(5)", "expects a table");
        add("a table other than the one Editor.Extension gave back",
            "Editor.Extension { name = \"Other\", apiVersion = 1 }\nreturn {}", "must return the table");
        add("nothing returned", "Editor.Extension { name = \"Nothing\", apiVersion = 1 }", "must return the table");
        return cases;
    }
}

TEST_F(ExtensionHostTest, EveryBadShapeIsAnErrorOfThatFileAloneAsData)
{
    for (const ShapeCase &shape : ShapeCases())
    {
        TempProject project;
        project.Write("a_bad.lua", shape.source);
        project.Write("b_good.lua", Valid("Good", "good.item"));
        ExtensionHost host(project.Root());
        host.Load();
        const json description = host.DescribeJson();
        EXPECT_EQ(StateOf(description, "a_bad.lua"), "failed") << shape.what;
        const std::string error = ErrorOf(description, "a_bad.lua");
        EXPECT_FALSE(error.empty()) << shape.what;
        EXPECT_NE(error.find(shape.expectedInError), std::string::npos) << shape.what << ": " << error;
        EXPECT_EQ(StateOf(description, "b_good.lua"), "loaded") << shape.what << ": the other file loads";
        EXPECT_TRUE(Find(description, "a_bad.lua").at("capabilities").empty()) << shape.what;
        EXPECT_EQ(host.UiJson().at("menuItems").size(), 1u) << shape.what << ": a bad file adds no menu item";
    }
}

TEST_F(ExtensionHostTest, TwoFilesThatDeclareTheSameItemIdLetTheFirstKeepIt)
{
    TempProject project;
    project.Write("a.lua", Valid("First", "shared.item"));
    project.Write("b.lua", Valid("Second", "shared.item"));
    project.Write("c.lua", Valid("Third", "own.item"));
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "b.lua"), "failed");
    EXPECT_NE(ErrorOf(description, "b.lua").find("already used by"), std::string::npos) << ErrorOf(description, "b.lua");
    EXPECT_EQ(StateOf(description, "c.lua"), "loaded");
    const json ui = host.UiJson();
    ASSERT_EQ(ui.at("menuItems").size(), 2u);
    EXPECT_EQ(ui.at("menuItems")[0].at("id"), "shared.item");
    EXPECT_EQ(ui.at("menuItems")[0].at("extension"), "a.lua");
    EXPECT_EQ(ui.at("menuItems")[1].at("id"), "own.item");
}

// ==================== The sandbox of a state ====================

TEST_F(ExtensionHostTest, AFileThatLoopsIsStoppedByItsBudgetAndTheHostKeepsAnswering)
{
    TempProject project;
    project.Write("a_loop.lua", "while true do end");
    project.Write("b_ok.lua", Valid("Ok", "ok.item"));
    ExtensionHost host(project.Root());
    const auto started = std::chrono::steady_clock::now();
    host.Load();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a_loop.lua"), "failed");
    const std::string error = ErrorOf(description, "a_loop.lua");
    EXPECT_TRUE(error.find("budget") != std::string::npos || error.find("time limit") != std::string::npos) << error;
    EXPECT_EQ(StateOf(description, "b_ok.lua"), "loaded");
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 1u);
}

TEST_F(ExtensionHostTest, AFileThatAsksForTooMuchMemoryFails)
{
    TempProject project;
    project.Write("a_memory.lua", "local s = string.rep(\"x\", 400000000)\nreturn Editor.Extension { name = \"Big\", apiVersion = 1 }");
    project.Write("b_ok.lua", Valid("Ok", "ok.item"));
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a_memory.lua"), "failed");
    EXPECT_FALSE(ErrorOf(description, "a_memory.lua").empty());
    EXPECT_EQ(StateOf(description, "b_ok.lua"), "loaded");
}

TEST_F(ExtensionHostTest, AStateHasNoWayToReachTheHostOrAnotherExtension)
{
    TempProject project;
    // Each of these is nil in the EditorExtension profile (E-X1's allowlist); a file that touches one fails, and a file
    // that only reads it gets nil. Another file's globals are not visible: states are separate.
    project.Write("a_globals.lua", R"LUA(
local absent = { "io", "os", "package", "debug", "require", "load", "loadstring", "dofile", "loadfile",
                 "collectgarbage", "print", "coroutine", "Application", "GameObject" }
local present = {}
for _, name in ipairs(absent) do
  if _G[name] ~= nil then present[#present + 1] = name end
end
if #present > 0 then error("reachable: " .. table.concat(present, ",")) end
if string.find ~= nil or string.gsub ~= nil then error("string patterns are reachable") end
Shared = "a"
return Editor.Extension { name = "Globals", apiVersion = 1 }
)LUA");
    project.Write("b_other.lua", R"LUA(
if Shared ~= nil then error("another extension's global is visible") end
local keys = {}
for k in next, _G do keys[#keys + 1] = k end
table.sort(keys)
-- Only the language's own names and the Editor namespace
for _, k in ipairs(keys) do
  if k == "Editor" then
    local editorKeys = {}
    for name in next, Editor do editorKeys[#editorKeys + 1] = name end
    if #editorKeys ~= 1 or editorKeys[1] ~= "Extension" then error("Editor has: " .. table.concat(editorKeys, ",")) end
  end
end
return Editor.Extension { name = "Other", apiVersion = 1 }
)LUA");
    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a_globals.lua"), "loaded") << ErrorOf(description, "a_globals.lua");
    EXPECT_EQ(StateOf(description, "b_other.lua"), "loaded") << ErrorOf(description, "b_other.lua");
}

// ==================== Reload ====================

TEST_F(ExtensionHostTest, ReloadPicksUpAnEditARemovedFileAndANewOne)
{
    TempProject project;
    project.Write("a.lua", Valid("Before", "a.item"));
    project.Write("b.lua", Valid("Removed", "b.item"));
    ExtensionHost host(project.Root());
    std::vector<std::uint32_t> told;
    host.SetChangedCallback([&told](const std::uint32_t revision) { told.push_back(revision); });
    host.Load();
    EXPECT_TRUE(told.empty()) << "the first load is not a change";
    const json first = host.DescribeJson();
    EXPECT_EQ(first.at("uiRevision"), 1);
    const std::string firstHash = Find(first, "a.lua").at("sha256").get<std::string>();
    EXPECT_EQ(Find(first, "a.lua").at("name"), "Before");

    project.Write("a.lua", Valid("After", "a.item"));
    project.Remove("b.lua");
    project.Write("c.lua", Valid("New", "c.item"));
    // Nothing happens on its own: the registry is what was loaded
    EXPECT_EQ(host.DescribeJson(), first);

    host.Reload();
    const json second = host.DescribeJson();
    EXPECT_EQ(second.at("uiRevision"), 2);
    EXPECT_EQ(told, (std::vector<std::uint32_t>{2}));
    EXPECT_EQ(Find(second, "a.lua").at("name"), "After");
    EXPECT_NE(Find(second, "a.lua").at("sha256").get<std::string>(), firstHash);
    EXPECT_TRUE(Find(second, "b.lua").is_null());
    EXPECT_EQ(StateOf(second, "c.lua"), "loaded");
    EXPECT_EQ(host.UiJson().at("uiRevision"), 2);

    // A file that broke is failed after a reload, and one that was repaired loads
    project.Write("a.lua", "error('now broken')");
    host.Reload();
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "failed");
    project.Write("a.lua", Valid("Fixed", "a.item"));
    host.Reload();
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded");
    EXPECT_EQ(host.UiRevision(), 4u);
    EXPECT_EQ(told, (std::vector<std::uint32_t>{2, 3, 4}));
}

TEST_F(ExtensionHostTest, ReloadClosesTheStatesItReplaces)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    project.Write("b.lua", Valid("B", "b.item"));
    const int before = Scripting::LuaSandbox::LiveStates();
    {
        ExtensionHost host(project.Root());
        host.Load();
        EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before + 2) << "one state per loaded file";
        host.Reload();
        host.Reload();
        EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before + 2) << "a reload replaces the states, it does not add";
        project.Remove("b.lua");
        host.Reload();
        EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before + 1);
        project.Write("a.lua", "error('broken')");
        host.Reload();
        EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before) << "a file that failed keeps no state";
    }
    EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before);
}

TEST_F(ExtensionHostTest, TheHostLeavesNoStateBehind)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    const int before = Scripting::LuaSandbox::LiveStates();
    {
        ExtensionHost host(project.Root());
        host.Load();
        EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before + 1);
    }
    EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), before);
}

// ==================== The fault flag ====================

TEST_F(ExtensionHostTest, ThreeFailuresInTenMinutesFaultAnExtensionUntilReload)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    project.Write("b.lua", Valid("B", "b.item"));
    ExtensionHost host(project.Root());
    std::vector<std::uint32_t> told;
    host.SetChangedCallback([&told](const std::uint32_t revision) { told.push_back(revision); });
    host.Load();

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(host.NoteFailure("a.lua", start), ExtensionState::Loaded);
    EXPECT_EQ(host.NoteFailure("a.lua", start + 1min), ExtensionState::Loaded);
    EXPECT_TRUE(told.empty());
    EXPECT_EQ(host.NoteFailure("a.lua", start + 2min), ExtensionState::Faulted);
    EXPECT_EQ(host.UiRevision(), 2u) << "the state changed, so the UI revision moves on";
    EXPECT_EQ(told, (std::vector<std::uint32_t>{2}));

    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a.lua"), "faulted");
    EXPECT_NE(ErrorOf(description, "a.lua").find("until reload"), std::string::npos) << ErrorOf(description, "a.lua");
    EXPECT_EQ(StateOf(description, "b.lua"), "loaded") << "another extension is not affected";
    // Its items are still listed, so a client can show them disabled with the reason
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);
    // A faulted extension is left alone by further failures
    EXPECT_EQ(host.NoteFailure("a.lua", start + 3min), ExtensionState::Faulted);
    EXPECT_EQ(host.UiRevision(), 2u);

    host.Reload();
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded") << "a reload clears the flag";
    EXPECT_EQ(host.NoteFailure("a.lua", start + 4min), ExtensionState::Loaded) << "and the count starts over";
}

TEST_F(ExtensionHostTest, FailuresFarApartDoNotFaultAnExtension)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    ExtensionHost host(project.Root());
    host.Load();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(host.NoteFailure("a.lua", start), ExtensionState::Loaded);
    EXPECT_EQ(host.NoteFailure("a.lua", start + 11min), ExtensionState::Loaded) << "the first is outside the window";
    EXPECT_EQ(host.NoteFailure("a.lua", start + 12min), ExtensionState::Loaded) << "two within the window";
    EXPECT_EQ(host.NoteFailure("a.lua", start + 13min), ExtensionState::Faulted) << "three within it";
    EXPECT_EQ(host.StateOf("a.lua"), ExtensionState::Faulted);
    EXPECT_EQ(host.StateOf("missing.lua"), ExtensionState::Failed);
    EXPECT_EQ(host.NoteFailure("missing.lua"), ExtensionState::Failed);
}

// ==================== Readers run no Lua ====================

TEST_F(ExtensionHostTest, TheReadersRunNoLua)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    project.Write("b.lua", Valid("B", "b.item"));
    project.Write("c.lua", "error('broken')");
    ExtensionHost host(project.Root());
    host.Load();
    ASSERT_EQ(host.LoadedCount(), 2u);

    // An instruction counter on every state the host kept. First it must work: one chunk run in each counts.
    int states = 0;
    g_instructions = 0;
    host.ForEachStateForTest([&states](const std::string &, lua_State *state)
    {
        lua_sethook(state, &CountInstructions, LUA_MASKCOUNT, 1);
        ++states;
        const int status = luaL_dostring(state, "local n = 0 for i = 1, 100 do n = n + i end");
        EXPECT_EQ(status, LUA_OK);
    });
    EXPECT_EQ(states, 2);
    EXPECT_GT(g_instructions.load(), 200) << "the counter sees Lua run";

    // Now the readers: nothing counted
    g_instructions = 0;
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(host.DescribeJson().empty());
        EXPECT_FALSE(host.UiJson().empty());
        EXPECT_FALSE(ExtensionHost::DisabledDescribeJson().empty());
        EXPECT_FALSE(ExtensionHost::DisabledUiJson().empty());
        (void)host.UiRevision();
        (void)host.StateOf("a.lua");
        (void)host.LoadedCount();
    }
    EXPECT_EQ(g_instructions.load(), 0) << "reading the registry ran Lua";
}

// ==================== Off the command thread, with an abandon deadline ====================

TEST_F(ExtensionHostTest, AWorkerThatNeverReturnsIsAbandonedAndTheHostKeepsAnswering)
{
    TempProject project;
    project.Write("a_ok.lua", Valid("A", "a.item"));
    project.Write("b_stall.lua", Valid("Stall", "stall.item"));
    project.Write("c_ok.lua", Valid("C", "c.item"));
    HoldWorkers("b_stall");
    ExtensionHost::SetAbandonMillisForTest(300);

    const int statesBefore = Scripting::LuaSandbox::LiveStates();
    ExtensionHost host(project.Root());
    const auto started = std::chrono::steady_clock::now();
    host.Load();
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_GE(took, 250ms) << "it waited for the worker";
    EXPECT_LT(took, 10s) << "and then gave up on it";

    const json description = host.DescribeJson();
    EXPECT_EQ(StateOf(description, "a_ok.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "b_stall.lua"), "failed");
    EXPECT_NE(ErrorOf(description, "b_stall.lua").find("abandoned"), std::string::npos) << ErrorOf(description, "b_stall.lua");
    EXPECT_EQ(StateOf(description, "c_ok.lua"), "loaded") << "the file after it still loads";
    EXPECT_EQ(ExtensionHost::LiveWorkers(), 1) << "the abandoned worker still exists, holding its slot";

    // The host answers, and a reload while the worker is still stuck does not start a second one for the same text
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);
    const auto reloadStarted = std::chrono::steady_clock::now();
    host.Reload();
    EXPECT_LT(std::chrono::steady_clock::now() - reloadStarted, 5s);
    EXPECT_EQ(ExtensionHost::LiveWorkers(), 1) << "no second worker for the same stuck text";
    EXPECT_EQ(StateOf(host.DescribeJson(), "b_stall.lua"), "failed");
    EXPECT_NE(ErrorOf(host.DescribeJson(), "b_stall.lua").find("still running"), std::string::npos)
        << ErrorOf(host.DescribeJson(), "b_stall.lua");

    // Let it go: it ends, and the state it made is closed by it (the host never took it)
    _gate->Open();
    EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; }));
    EXPECT_TRUE(WaitUntil([&] { return Scripting::LuaSandbox::LiveStates() == statesBefore + 2; }))
        << "only the two loaded extensions keep a state";

    // Once it has finished, the same file is tried again at the next reload
    EXPECT_TRUE(WaitUntil([&]
    {
        host.Reload();
        return StateOf(host.DescribeJson(), "b_stall.lua") == "loaded";
    }));
}

TEST_F(ExtensionHostTest, NoMoreThanFourWorkersAtOnceAbandonedOnesCounted)
{
    TempProject project;
    for (int i = 1; i <= 5; ++i)
    {
        project.Write("stall" + std::to_string(i) + ".lua", Valid("S" + std::to_string(i), "s" + std::to_string(i)));
    }
    project.Write("z_ok.lua", Valid("Ok", "ok.item"));
    HoldWorkers("stall");
    ExtensionHost::SetAbandonMillisForTest(200);

    ExtensionHost host(project.Root());
    host.Load();
    const json description = host.DescribeJson();
    int abandoned = 0;
    int refused = 0;
    for (int i = 1; i <= 5; ++i)
    {
        const std::string id = "stall" + std::to_string(i) + ".lua";
        EXPECT_EQ(StateOf(description, id), "failed") << id;
        const std::string error = ErrorOf(description, id);
        abandoned += error.find("took longer") != std::string::npos ? 1 : 0;
        refused += error.find("still running") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(abandoned, ExtensionHost::MaxWorkers);
    EXPECT_EQ(refused, 1) << "the fifth found no free worker and failed at once";
    EXPECT_EQ(ExtensionHost::LiveWorkers(), ExtensionHost::MaxWorkers);
    EXPECT_EQ(StateOf(description, "z_ok.lua"), "failed") << "no worker is free for it either";

    _gate->Open();
    EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; }));
    // A worker is gone a moment before its job is released, so the reload is tried until it sees them all gone
    EXPECT_TRUE(WaitUntil([&]
    {
        host.Reload();
        return host.LoadedCount() == 6u;
    })) << "with the workers gone every file loads";
}

TEST_F(ExtensionHostTest, ShutdownWaitsABoundedTimeForWorkersAndStopsNewOnes)
{
    TempProject project;
    project.Write("stall.lua", Valid("Stall", "stall.item"));
    HoldWorkers("stall");
    ExtensionHost::SetAbandonMillisForTest(200);

    ExtensionHost host(project.Root());
    host.Load();
    ASSERT_EQ(ExtensionHost::LiveWorkers(), 1);

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(host.Shutdown(200ms), 1) << "one worker is still running when it gives up";
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

    // After Shutdown nothing new starts
    project.Write("late.lua", Valid("Late", "late.item"));
    host.Load();
    EXPECT_EQ(StateOf(host.DescribeJson(), "late.lua"), "failed");
    EXPECT_NE(ErrorOf(host.DescribeJson(), "late.lua").find("shutting down"), std::string::npos);

    _gate->Open();
    EXPECT_EQ(host.Shutdown(10s), 0) << "and once it ends, Shutdown reports none left";
}

TEST_F(ExtensionHostTest, ShutdownWithNoWorkersReturnsAtOnce)
{
    TempProject project;
    project.Write("a.lua", Valid("A", "a.item"));
    ExtensionHost host(project.Root());
    host.Load();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(host.Shutdown(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(host.Shutdown(), 0) << "idempotent";
}
