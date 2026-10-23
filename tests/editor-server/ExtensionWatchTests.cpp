#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/EditorServer.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/ExtensionHost.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// The editor/ folder watcher (--editor-extensions-watch, E-X6): off by default; polls the folder like the asset watcher
// (modification time and size, then a content hash) and loads again only the file that changed, leaving the other states
// and their failure counts alone; a file whose bytes did not change is not loaded again; a new file is loaded and a gone
// one dropped; a file that failed is not tried again until it changes; each change moves the uiRevision once and pushes
// extensionsChanged; nothing outside editor/ (a link, another folder, another extension) is ever read. Through
// EditorServer::PollEditorExtensionsIfDue (the call ProcessCommands makes) and through ExtensionHost::ReloadChanged.
namespace
{
    class TempProject
    {
    public:
        TempProject() : _base(fs::file_time_type::clock::now())
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-watch-" + std::to_string(stamp) + "-" + std::to_string(counter++));
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

        // Writes the file and gives it a modification time later than any this project gave a file before, so the change
        // is seen whatever the clock's resolution and however fast the test goes
        void Write(const std::string &relative, const std::string &text)
        {
            WriteIn("editor", relative, text);
        }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            WriteIn("assets", relative, text);
        }

        void Remove(const std::string &relative) const
        {
            std::error_code error;
            fs::remove(_root / "editor" / relative, error);
        }

    private:
        void WriteIn(const char *folder, const std::string &relative, const std::string &text)
        {
            const fs::path path = _root / folder / relative;
            fs::create_directories(path.parent_path());
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << text;
            }
            ++_version;
            fs::last_write_time(path, _base + std::chrono::seconds(10 * _version));
        }

        fs::path _root;
        fs::file_time_type _base;
        int _version = 0;
    };

    // A file with one extension that has one item
    std::string Extension(const std::string &name, const std::string &itemId)
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = {},\n"
               "  menu = {\n"
               "    { id = \"" + itemId + "\", path = \"Tools/" + name + "\", run = function(ctx, p) end },\n"
               "  },\n"
               "}\n";
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

    std::string NameOf(const json &description, const std::string &id)
    {
        const json found = Find(description, id);
        return found.is_null() ? std::string("(absent)") : found.at("name").get<std::string>();
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

    std::vector<std::string> Ids(const json &description)
    {
        std::vector<std::string> ids;
        for (const json &extension : description.at("extensions"))
            ids.push_back(extension.at("id").get<std::string>());
        return ids;
    }

    // The Lua state each extension keeps: a state that was not loaded again is the very same one
    std::map<std::string, const void *> States(const ExtensionHost &host)
    {
        std::map<std::string, const void *> states;
        host.ForEachStateForTest([&states](const std::string &id, lua_State *state) { states[id] = state; });
        return states;
    }

    std::vector<json> EventsOfKind(const EditorServer &server, const std::string &kind)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            if (event.value("kind", "") == kind)
                found.push_back(event);
        }
        return found;
    }

    class ExtensionWatchTest : public ::testing::Test
    {
    protected:
        void SetUp() override { _watchdog.emplace(std::chrono::seconds(120)); }

        void TearDown() override
        {
            ExtensionHost::SetMaxTotalBytesForTest(0);
            _watchdog.reset();
        }

        // The extensions on, the watcher on, and a check on every call
        static void StartWatching(EditorServer &server, const TempProject &project)
        {
            ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
            server.SetEditorExtensionsWatching(true);
            server.SetEditorExtensionsPollInterval(0ms);
        }

        std::optional<Tests::HangWatchdog> _watchdog;
    };
}

TEST_F(ExtensionWatchTest, TheWatcherIsOffByDefaultAndChangesNothingWhileOff)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_FALSE(server.IsWatchingEditorExtensions());
    const uint32_t revision = server.GetEditorExtensions()->UiRevision();

    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(server.GetEditorExtensions()->DescribeJson(), "a.lua"), "Alpha");
    EXPECT_EQ(server.GetEditorExtensions()->ExtensionCount(), 1u);
    EXPECT_EQ(server.GetEditorExtensions()->UiRevision(), revision);
    EXPECT_TRUE(EventsOfKind(server, "extensionsChanged").empty());

    // Switched on, it sees what changed meanwhile; switched off again, it stops
    server.SetEditorExtensionsWatching(true);
    server.SetEditorExtensionsPollInterval(0ms);
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(server.GetEditorExtensions()->DescribeJson(), "a.lua"), "Alpha Two");
    server.SetEditorExtensionsWatching(false);
    project.Write("a.lua", Extension("Alpha Three", "a.item"));
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(server.GetEditorExtensions()->DescribeJson(), "a.lua"), "Alpha Two");
}

TEST_F(ExtensionWatchTest, AHostWithoutExtensionsHasNothingToWatch)
{
    EditorServer server;
    server.SetEditorExtensionsWatching(true);
    server.SetEditorExtensionsPollInterval(0ms);
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(server.GetEditorExtensions(), nullptr);
}

TEST_F(ExtensionWatchTest, OnlyTheChangedFileIsLoadedAgain)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    ASSERT_EQ(host.LoadedCount(), 2u);
    const std::map<std::string, const void *> before = States(host);
    ASSERT_EQ(before.size(), 2u);
    const uint32_t revision = host.UiRevision();

    EXPECT_FALSE(server.PollEditorExtensionsIfDue()) << "nothing changed";
    EXPECT_EQ(host.UiRevision(), revision);

    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    const json description = host.DescribeJson();
    EXPECT_EQ(NameOf(description, "a.lua"), "Alpha Two");
    EXPECT_EQ(NameOf(description, "b.lua"), "Beta");
    EXPECT_EQ(StateOf(description, "a.lua"), "loaded");
    EXPECT_EQ(StateOf(description, "b.lua"), "loaded");
    const std::map<std::string, const void *> after = States(host);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_NE(after.at("a.lua"), before.at("a.lua")) << "the changed file has a new state";
    EXPECT_EQ(after.at("b.lua"), before.at("b.lua")) << "the other file's state is the one it had";

    // One change: the uiRevision moved once and the clients were told once
    EXPECT_EQ(host.UiRevision(), revision + 1);
    const std::vector<json> changed = EventsOfKind(server, "extensionsChanged");
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].at("uiRevision"), revision + 1);
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);

    // And the next check finds nothing
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.UiRevision(), revision + 1);
    EXPECT_EQ(EventsOfKind(server, "extensionsChanged").size(), 1u);
}

TEST_F(ExtensionWatchTest, AFileWhoseBytesDidNotChangeIsNotLoadedAgain)
{
    TempProject project;
    const std::string text = Extension("Alpha", "a.item");
    project.Write("a.lua", text);
    project.Write("b.lua", Extension("Beta", "b.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    const std::map<std::string, const void *> before = States(host);
    const uint32_t revision = host.UiRevision();

    // The file is written again with the same bytes (a touch, a save that changed nothing): its modification time moved,
    // its content hash did not
    project.Write("a.lua", text);
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(States(host), before) << "no state was made again";
    EXPECT_EQ(host.UiRevision(), revision);
    EXPECT_TRUE(EventsOfKind(server, "extensionsChanged").empty());

    // The new modification time was recorded: a later real change is still seen, once
    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(host.DescribeJson(), "a.lua"), "Alpha Two");
    EXPECT_EQ(host.UiRevision(), revision + 1);
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
}

TEST_F(ExtensionWatchTest, ANewFileIsLoadedAndAGoneOneIsDropped)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    const std::map<std::string, const void *> before = States(host);
    const uint32_t revision = host.UiRevision();

    project.Write("c.lua", Extension("Gamma", "c.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua", "b.lua", "c.lua"}));
    EXPECT_EQ(StateOf(host.DescribeJson(), "c.lua"), "loaded");
    EXPECT_EQ(host.UiRevision(), revision + 1);
    EXPECT_EQ(States(host).at("a.lua"), before.at("a.lua"));
    EXPECT_EQ(States(host).at("b.lua"), before.at("b.lua"));

    project.Remove("b.lua");
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua", "c.lua"}));
    EXPECT_EQ(host.UiRevision(), revision + 2);
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);

    // Both at once: one revision, one event
    project.Write("d.lua", Extension("Delta", "d.item"));
    project.Remove("a.lua");
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"c.lua", "d.lua"}));
    EXPECT_EQ(host.UiRevision(), revision + 3);
    EXPECT_EQ(EventsOfKind(server, "extensionsChanged").size(), 3u);
}

TEST_F(ExtensionWatchTest, AFileThatFailedIsNotTriedAgainUntilItChanges)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();

    project.Write("broken.lua", "this is not lua (((\n");
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "broken.lua"), "failed");
    EXPECT_FALSE(ErrorOf(host.DescribeJson(), "broken.lua").empty());
    const uint32_t revision = host.UiRevision();

    // Nothing changed since: the same failure is not loaded (and logged) again every second
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.UiRevision(), revision);

    // Fixed, it loads
    project.Write("broken.lua", Extension("Fixed", "fixed.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "broken.lua"), "loaded");
    EXPECT_EQ(NameOf(host.DescribeJson(), "broken.lua"), "Fixed");
    EXPECT_EQ(host.LoadedCount(), 2u);
}

TEST_F(ExtensionWatchTest, AFileThatIsTooLargeFailsOnceAndIsNotReadAgain)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();

    project.Write("huge.lua", "-- " + std::string(ExtensionHost::MaxFileBytes + 1, 'x') + "\nreturn nil\n");
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "huge.lua"), "failed");
    EXPECT_NE(ErrorOf(host.DescribeJson(), "huge.lua").find("larger than"), std::string::npos);
    const uint32_t revision = host.UiRevision();
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.UiRevision(), revision);
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded");
}

TEST_F(ExtensionWatchTest, ALoadedAgainFileCannotTakeAnIdAnotherExtensionHas)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "shared.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    const std::map<std::string, const void *> before = States(host);

    project.Write("b.lua", Extension("Beta", "shared.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");
    EXPECT_NE(ErrorOf(host.DescribeJson(), "b.lua").find("already used by the extension a.lua"), std::string::npos)
        << ErrorOf(host.DescribeJson(), "b.lua");
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded");
    EXPECT_EQ(States(host).at("a.lua"), before.at("a.lua")) << "the extension that had the id keeps it, untouched";
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 1u);

    // Given its own id again, it loads
    project.Write("b.lua", Extension("Beta", "b.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "loaded");
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);
}

// A file that failed on an id another extension held is tried again when that extension changes or goes
TEST_F(ExtensionWatchTest, AFileThatFailedOnAnIdClashLoadsWhenTheOwnerGivesTheIdUp)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "shared.item"));
    project.Write("b.lua", Extension("Beta", "shared.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    ASSERT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");
    ASSERT_NE(ErrorOf(host.DescribeJson(), "b.lua").find("already used by the extension a.lua"), std::string::npos);

    // b.lua itself is untouched: the owner changing is what lets it load
    project.Write("a.lua", Extension("Alpha", "a.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded");
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "loaded") << ErrorOf(host.DescribeJson(), "b.lua");
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 2u);
    EXPECT_FALSE(server.PollEditorExtensionsIfDue()) << "nothing moved since: no further reload";
}

TEST_F(ExtensionWatchTest, AFileThatFailedOnAnIdClashLoadsWhenTheOwnerIsRemoved)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "shared.item"));
    project.Write("b.lua", Extension("Beta", "shared.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    ASSERT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");

    project.Remove("a.lua");
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "(absent)");
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "loaded") << ErrorOf(host.DescribeJson(), "b.lua");
    EXPECT_EQ(host.UiJson().at("menuItems").size(), 1u);
}

TEST_F(ExtensionWatchTest, AFileThatFailedOnAnIdClashIsNotTriedAgainWhileTheOwnerIsUnchangedOrKeepsTheId)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "shared.item"));
    project.Write("b.lua", Extension("Beta", "shared.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    ASSERT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");
    const uint32_t revision = host.UiRevision();

    // Nothing moved, and an unrelated file is added: b.lua is not loaded again (it was not counted as reloaded)
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.UiRevision(), revision);
    project.Write("c.lua", Extension("Gamma", "c.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");
    EXPECT_EQ(StateOf(host.DescribeJson(), "c.lua"), "loaded");

    // The owner changes but keeps the id: b.lua is tried once, fails on the id again, and waits for the owner's next change
    project.Write("a.lua", Extension("Alpha Two", "shared.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(StateOf(host.DescribeJson(), "a.lua"), "loaded");
    EXPECT_EQ(StateOf(host.DescribeJson(), "b.lua"), "failed");
    EXPECT_NE(ErrorOf(host.DescribeJson(), "b.lua").find("already used by the extension a.lua"), std::string::npos);
    const uint32_t settled = host.UiRevision();
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.UiRevision(), settled);
}

TEST_F(ExtensionWatchTest, TheOtherExtensionsKeepTheirFailureCountsWhenOneFileIsLoadedAgain)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();

    EXPECT_EQ(host.NoteFailure("a.lua"), ExtensionState::Loaded);
    EXPECT_EQ(host.NoteFailure("a.lua"), ExtensionState::Loaded);
    project.Write("b.lua", Extension("Beta Two", "b.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(host.DescribeJson(), "b.lua"), "Beta Two");
    // Alpha's third failure within ten minutes: the two before the reload of Beta still count
    EXPECT_EQ(host.NoteFailure("a.lua"), ExtensionState::Faulted);

    // A faulted extension stays faulted while the others change, and a change of its own file starts it afresh
    project.Write("b.lua", Extension("Beta Three", "b.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.StateOf("a.lua"), ExtensionState::Faulted);
    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(host.StateOf("a.lua"), ExtensionState::Loaded);
    EXPECT_EQ(NameOf(host.DescribeJson(), "a.lua"), "Alpha Two");
}

TEST_F(ExtensionWatchTest, NothingOutsideTheEditorFolderIsEverLoaded)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    // A Lua file in the assets folder, a file that is not Lua in editor/
    project.WriteAsset("scripts/Evil.lua", Extension("Evil", "evil.item"));
    project.Write("notes.txt", Extension("Notes", "notes.item"));
    EditorServer server;
    StartWatching(server, project);
    ExtensionHost &host = *server.GetEditorExtensions();
    ASSERT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua"}));
    const uint32_t revision = host.UiRevision();

    project.WriteAsset("scripts/Evil2.lua", Extension("Evil Two", "evil2.item"));
    project.Write("more.txt", Extension("More", "more.item"));
    EXPECT_FALSE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua"}));
    EXPECT_EQ(host.UiRevision(), revision);

    // A link out of editor/ (to a folder, to a file) is not followed
    const fs::path outside = project.Root().parent_path() / (project.Root().filename().string() + "-outside");
    std::error_code error;
    fs::create_directories(outside, error);
    {
        std::ofstream file(outside / "linked.lua", std::ios::binary);
        file << Extension("Linked", "linked.item");
    }
    const fs::path folderLink = project.Root() / "editor" / "linked-folder";
    const fs::path fileLink = project.Root() / "editor" / "linked-file.lua";
    std::error_code linkError;
    fs::create_directory_symlink(outside, folderLink, linkError);
    std::error_code fileLinkError;
    fs::create_symlink(outside / "linked.lua", fileLink, fileLinkError);
    if (!linkError || !fileLinkError)
    {
        EXPECT_FALSE(server.PollEditorExtensionsIfDue());
        EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua"})) << "a link is not followed";
        EXPECT_EQ(host.UiRevision(), revision);
    }
    std::error_code removeError;
    fs::remove_all(outside, removeError);
}

TEST_F(ExtensionWatchTest, TheWatcherChecksOnItsIntervalAndNeedsNoClient)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    server.SetEditorExtensionsWatching(true);
    auto now = std::chrono::steady_clock::now();
    server.SetAssetPollClock([&now] { return now; });
    server.SetEditorExtensionsPollInterval(10s);
    ASSERT_FALSE(server.HasClient()) << "no client is connected: the editor/ watcher does not wait for one";
    ExtensionHost &host = *server.GetEditorExtensions();

    EXPECT_FALSE(server.PollEditorExtensionsIfDue()) << "the first check, nothing changed";
    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    now += 5s;
    EXPECT_FALSE(server.PollEditorExtensionsIfDue()) << "the interval has not passed";
    EXPECT_EQ(NameOf(host.DescribeJson(), "a.lua"), "Alpha");
    now += 6s;
    EXPECT_TRUE(server.PollEditorExtensionsIfDue());
    EXPECT_EQ(NameOf(host.DescribeJson(), "a.lua"), "Alpha Two");
    server.SetAssetPollClock({});
}

TEST_F(ExtensionWatchTest, ReloadChangedSaysWhatItDid)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    project.Write("b.lua", Extension("Beta", "b.item"));
    project.Write("c.lua", Extension("Gamma", "c.item"));
    ExtensionHost host(project.Root());
    host.Load();
    ASSERT_EQ(host.LoadedCount(), 3u);
    const uint32_t revision = host.UiRevision();

    ExtensionHost::WatchOutcome none = host.ReloadChanged();
    EXPECT_FALSE(none.Changed());
    EXPECT_EQ(none.reloaded + none.added + none.removed + none.touched, 0u);

    project.Write("a.lua", Extension("Alpha Two", "a.item")); // changed
    project.Write("b.lua", Extension("Beta", "b.item"));      // the same bytes
    project.Remove("c.lua");                                   // gone
    project.Write("d.lua", Extension("Delta", "d.item"));     // new
    const ExtensionHost::WatchOutcome outcome = host.ReloadChanged();
    EXPECT_TRUE(outcome.Changed());
    EXPECT_EQ(outcome.reloaded, 1u);
    EXPECT_EQ(outcome.touched, 1u);
    EXPECT_EQ(outcome.removed, 1u);
    EXPECT_EQ(outcome.added, 1u);
    EXPECT_EQ(Ids(host.DescribeJson()), (std::vector<std::string>{"a.lua", "b.lua", "d.lua"}));
    EXPECT_EQ(host.UiRevision(), revision + 1) << "one revision for the whole check";

    // A check where only a touch happened changes no revision and tells nobody
    int told = 0;
    host.SetChangedCallback([&told](std::uint32_t) { ++told; });
    project.Write("b.lua", Extension("Beta", "b.item"));
    const ExtensionHost::WatchOutcome touched = host.ReloadChanged();
    EXPECT_FALSE(touched.Changed());
    EXPECT_EQ(touched.touched, 1u);
    EXPECT_EQ(told, 0);
    EXPECT_EQ(host.UiRevision(), revision + 1);

    project.Write("d.lua", Extension("Delta Two", "d.item"));
    EXPECT_TRUE(host.ReloadChanged().Changed());
    EXPECT_EQ(told, 1) << "the changed callback is told once";
}

TEST_F(ExtensionWatchTest, AStoppedHostChecksNothing)
{
    TempProject project;
    project.Write("a.lua", Extension("Alpha", "a.item"));
    ExtensionHost host(project.Root());
    host.Load();
    (void)host.Shutdown();
    project.Write("a.lua", Extension("Alpha Two", "a.item"));
    EXPECT_FALSE(host.ReloadChanged().Changed());
    EXPECT_EQ(NameOf(host.DescribeJson(), "a.lua"), "Alpha");
}
