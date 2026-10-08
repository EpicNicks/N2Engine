#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <engine/Version.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    json ValidProjectJson()
    {
        return {
            {"formatVersion", 1},
            {"name", "My Game"},
            {"projectId", "8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10"},
            {"engineVersion", "1.0.0"},
            {"startupScene", "res://scenes/Main.scene"},
            {"scenes", {"res://scenes/Main.scene", "res://scenes/Level2.scene"}},
            {"settings", {{"physics", {{"fixedTimestep", 0.02}}}, {"window", {{"title", "My Game"}}}}},
        };
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    /// A fresh folder under the temp directory for each test, removed afterwards
    class ProjectFileTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-project-file-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        fs::path _root;
    };
}

// ==================== Parse and validate ====================

TEST(ProjectFileParseTest, ReadsEveryKnownKey)
{
    const auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project) << project.error();
    EXPECT_EQ(project->formatVersion, 1);
    EXPECT_EQ(project->name, "My Game");
    EXPECT_EQ(project->projectId.ToString(), "8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10");
    EXPECT_EQ(project->engineVersion, "1.0.0");
    EXPECT_EQ(project->startupScene, "res://scenes/Main.scene");
    EXPECT_EQ(project->scenes, (std::vector<std::string>{"res://scenes/Main.scene", "res://scenes/Level2.scene"}));
    EXPECT_EQ(project->settings.at("physics").at("fixedTimestep").get<double>(), 0.02);
    EXPECT_TRUE(project->unknownKeys.empty());
}

TEST(ProjectFileParseTest, OptionalKeysDefault)
{
    json j = ValidProjectJson();
    j.erase("startupScene");
    j.erase("scenes");
    j.erase("settings");
    const auto project = IO::ProjectFile::FromJson(j);
    ASSERT_TRUE(project) << project.error();
    EXPECT_TRUE(project->startupScene.empty());
    EXPECT_TRUE(project->scenes.empty());
    EXPECT_EQ(project->settings, json::object());
}

TEST(ProjectFileParseTest, RoundTripKeepsUnknownKeysAtTheTopAndInSettings)
{
    json j = ValidProjectJson();
    j["editorHost"] = "build/bin/MyGameEditorHost.exe";
    j["future"] = {{"nested", {1, 2, 3}}};
    j["settings"]["someFutureBlock"] = {{"x", true}};

    const auto project = IO::ProjectFile::FromJson(j);
    ASSERT_TRUE(project) << project.error();
    EXPECT_EQ(project->unknownKeys, (json{{"editorHost", "build/bin/MyGameEditorHost.exe"},
                                          {"future", {{"nested", {1, 2, 3}}}}}));
    EXPECT_EQ(project->ToJson(), j) << "nothing read is lost when it is written back";

    const auto again = IO::ProjectFile::Parse(project->ToText());
    ASSERT_TRUE(again) << again.error();
    EXPECT_EQ(again->ToJson(), j);
}

TEST(ProjectFileParseTest, AKnownKeyInUnknownKeysDoesntOverrideTheRealValue)
{
    auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project);
    project->unknownKeys["name"] = "Not this";
    EXPECT_EQ(project->ToJson().at("name"), "My Game");
}

TEST(ProjectFileParseTest, TheTextIsIndentedSortedAndEndsInANewline)
{
    const auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project);
    const std::string text = project->ToText();
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.rfind("{\n  \"engineVersion\"", 0), 0u) << text;
    EXPECT_LT(text.find("\"formatVersion\""), text.find("\"name\""));
}

TEST(ProjectFileParseTest, TheIdIsWrittenInLowerCase)
{
    json j = ValidProjectJson();
    j["projectId"] = "8E0C3A8E-0B1F-4F5E-9D0E-3F6F1C7D2A10";
    const auto project = IO::ProjectFile::FromJson(j);
    ASSERT_TRUE(project) << project.error();
    EXPECT_EQ(project->ToJson().at("projectId"), "8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10");
}

TEST(ProjectFileParseTest, ValidationErrorsNameTheKey)
{
    struct Case
    {
        const char *key;
        json value; // null: the key is removed
    };
    const Case cases[] = {
        {"formatVersion", nullptr},
        {"formatVersion", "1"},
        {"formatVersion", 0},
        {"formatVersion", 2},
        {"formatVersion", 1.5},
        {"name", nullptr},
        {"name", ""},
        {"name", "   "},
        {"name", 7},
        {"projectId", nullptr},
        {"projectId", "not a uuid"},
        {"projectId", "00000000-0000-0000-0000-000000000000"},
        {"engineVersion", nullptr},
        {"engineVersion", "1.0"},
        {"engineVersion", "v1.0.0"},
        {"startupScene", 3},
        {"startupScene", "scenes/Main.scene"},
        {"startupScene", "res://scenes/Main.json"},
        {"startupScene", "res://../outside.scene"},
        {"startupScene", "user://Main.scene"},
        {"scenes", "res://scenes/Main.scene"},
        {"scenes", {1}},
        {"scenes", {"res://scenes/notascene.txt"}},
        {"settings", {1, 2}},
        {"settings", "x"},
    };
    for (const Case &c : cases)
    {
        json j = ValidProjectJson();
        if (c.value.is_null())
            j.erase(c.key);
        else
            j[c.key] = c.value;
        const auto project = IO::ProjectFile::FromJson(j);
        ASSERT_FALSE(project) << c.key << " = " << c.value.dump();
        EXPECT_EQ(project.error().rfind(std::string(c.key) + ":", 0), 0u) << project.error();
    }
}

TEST(ProjectFileParseTest, ANewerFormatIsRefusedSayingSo)
{
    json j = ValidProjectJson();
    j["formatVersion"] = IO::ProjectFile::CurrentFormatVersion + 1;
    const auto project = IO::ProjectFile::FromJson(j);
    ASSERT_FALSE(project);
    EXPECT_NE(project.error().find("newer"), std::string::npos) << project.error();
}

TEST(ProjectFileParseTest, TextThatIsntAnObjectIsAnError)
{
    EXPECT_FALSE(IO::ProjectFile::Parse("{ not json"));
    EXPECT_FALSE(IO::ProjectFile::Parse("[]"));
    EXPECT_FALSE(IO::ProjectFile::Parse(""));
}

TEST(ProjectFileParseTest, ScenePathsAreResPathsToSceneFiles)
{
    for (const char *path : {"res://Main.scene", "res://scenes/Main.scene", "res://a/b/Level 2.SCENE",
                             "res://scenes/../Main.scene"})
    {
        EXPECT_TRUE(IO::ProjectFile::IsScenePath(path)) << path;
    }
    for (const char *path : {"", "res://", "res://.scene", "scenes/Main.scene", "user://Main.scene",
                             "C:/game/assets/Main.scene", "res://../Main.scene", "res://a/../../Main.scene",
                             "res://Main.scene.json", "res://scenes/"})
    {
        EXPECT_FALSE(IO::ProjectFile::IsScenePath(path)) << path;
    }
}

TEST(ProjectFileParseTest, EngineVersionsCompareByMajorThenMinor)
{
    using IO::EngineVersionMatch;
    EXPECT_EQ(IO::CompareEngineVersions("1.0.0", "1.0.0"), EngineVersionMatch::Compatible);
    EXPECT_EQ(IO::CompareEngineVersions("1.0.3", "1.0.0"), EngineVersionMatch::Compatible);
    EXPECT_EQ(IO::CompareEngineVersions("1.2.0", "1.0.0"), EngineVersionMatch::MinorDiffers);
    EXPECT_EQ(IO::CompareEngineVersions("2.0.0", "1.0.0"), EngineVersionMatch::MajorDiffers);
    EXPECT_EQ(IO::CompareEngineVersions("1.0", "1.0.0"), EngineVersionMatch::Invalid);
    EXPECT_EQ(IO::CompareEngineVersions("1.0.0", "unknown"), EngineVersionMatch::Invalid);
    EXPECT_TRUE(IO::IsEngineVersion(EngineVersion())) << EngineVersion();
}

// ==================== user:// ====================

TEST(ProjectFileParseTest, TheUserDataFolderIsTheSanitisedNameAndTheIdsFirstEightDigits)
{
    auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project);
    EXPECT_EQ(project->UserDataPath("base"), fs::path("base") / "My_Game-8e0c3a8e");
    EXPECT_EQ(project->UserDataPath().parent_path(), IO::ProjectFile::UserDataBase());

    // Two projects with one name don't share a folder
    auto other = *project;
    other.projectId = Math::UUID::FromString("12345678-0b1f-4f5e-9d0e-3f6f1c7d2a10").value();
    EXPECT_NE(other.UserDataPath("base"), project->UserDataPath("base"));
}

TEST(ProjectFileParseTest, FolderNamesKeepOnlySafeCharacters)
{
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("My Game"), "My_Game");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("a/b\\c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("..hidden.."), "hidden");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("v1.2-final_cut"), "v1.2-final_cut");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName(""), "Project");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("..."), "Project");
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName("\xC3\xA9t\xC3\xA9"), "t") << "non-ASCII bytes become '_'";
    EXPECT_EQ(IO::ProjectFile::SanitizeFolderName(std::string(100, 'x')).size(), 48u);
}

// ==================== Load and save ====================

TEST_F(ProjectFileTest, SaveThenLoadRoundTrips)
{
    json j = ValidProjectJson();
    j["future"] = "kept";
    const auto project = IO::ProjectFile::FromJson(j);
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->Save(_root));

    EXPECT_FALSE(fs::exists(IO::ProjectFile::PathIn(_root).string() + ".tmp")) << "the temporary file is renamed";
    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), project->ToText());

    const auto loaded = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(loaded->ToJson(), j);
}

TEST_F(ProjectFileTest, SaveReplacesTheFile)
{
    auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->Save(_root));
    project->name = "Renamed";
    ASSERT_TRUE(project->Save(_root));
    const auto loaded = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(loaded->name, "Renamed");
}

TEST_F(ProjectFileTest, AnInvalidProjectIsNotSaved)
{
    auto project = IO::ProjectFile::FromJson(ValidProjectJson());
    ASSERT_TRUE(project);
    project->name.clear();
    const auto saved = project->Save(_root);
    ASSERT_FALSE(saved);
    EXPECT_NE(saved.error().find("name"), std::string::npos) << saved.error();
    EXPECT_FALSE(fs::exists(IO::ProjectFile::PathIn(_root)));
}

TEST_F(ProjectFileTest, LoadingAFolderWithoutAProjectFileSaysHowToMakeOne)
{
    const auto loaded = IO::ProjectFile::Load(_root);
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().find("project.n2proj not found"), std::string::npos) << loaded.error();
    EXPECT_NE(loaded.error().find("--create"), std::string::npos) << loaded.error();
}

TEST_F(ProjectFileTest, LoadingACorruptFileNamesTheFile)
{
    std::ofstream(IO::ProjectFile::PathIn(_root)) << "{ \"formatVersion\": 1, ";
    const auto loaded = IO::ProjectFile::Load(_root);
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().find("project.n2proj"), std::string::npos) << loaded.error();
    EXPECT_NE(loaded.error().find("not valid JSON"), std::string::npos) << loaded.error();
}

// ==================== CreateProject ====================

TEST_F(ProjectFileTest, CreateProjectMakesTheLayout)
{
    const fs::path dir = _root / "My Game";
    const auto created = IO::CreateProject(dir);
    ASSERT_TRUE(created) << created.error().message;

    EXPECT_EQ(created->name, "My Game") << "the folder's name by default";
    EXPECT_NE(created->projectId, Math::UUID::ZERO);
    EXPECT_EQ(created->engineVersion, EngineVersion());
    EXPECT_EQ(created->startupScene, IO::DefaultStartupScene);
    EXPECT_EQ(created->scenes, (std::vector<std::string>{std::string(IO::DefaultStartupScene)}));
    EXPECT_EQ(created->settings, json::object());

    const auto loaded = IO::ProjectFile::Load(dir);
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(loaded->ToJson(), created->ToJson());

    // An empty scene, as Scene::Serialize writes one
    const json scene = json::parse(ReadFile(dir / "assets" / "scenes" / "Main.scene"));
    EXPECT_EQ(scene, (json{{"name", "Main"}, {"rootGameObjects", json::array()}}));

    // A script in the engine's model (a class table with SerializableFields, returned from the chunk)
    const std::string script = ReadFile(dir / "assets" / "scripts" / "Example.lua");
    EXPECT_NE(script.find("Example.SerializableFields"), std::string::npos) << script;
    EXPECT_NE(script.find("return Example"), std::string::npos) << script;

    EXPECT_NE(ReadFile(dir / ".gitignore").find(".n2/"), std::string::npos);
}

TEST_F(ProjectFileTest, CreateProjectTakesANameAndAnId)
{
    const Math::UUID id = Math::UUID::FromString("8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10").value();
    const auto created = IO::CreateProject(_root, {.name = "Named", .projectId = id});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->name, "Named");
    EXPECT_EQ(created->projectId, id);
}

TEST_F(ProjectFileTest, TwoNewProjectsGetDifferentIds)
{
    const auto first = IO::CreateProject(_root / "a");
    const auto second = IO::CreateProject(_root / "b");
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->projectId, second->projectId);
}

TEST_F(ProjectFileTest, CreateProjectRefusesAFolderThatIsAlreadyAProject)
{
    ASSERT_TRUE(IO::CreateProject(_root, {.name = "First"}));
    const std::string before = ReadFile(IO::ProjectFile::PathIn(_root));

    const auto again = IO::CreateProject(_root, {.name = "Second"});
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().kind, IO::CreateProjectErrorKind::AlreadyAProject);
    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), before) << "nothing is written";
}

TEST_F(ProjectFileTest, CreateProjectAdoptsAFolderKeepingItsFiles)
{
    // An existing folder with a scene of its own where Main.scene goes, and assets referenced by UUID
    fs::create_directories(_root / "assets" / "scenes");
    std::ofstream(_root / "assets" / "scenes" / "Main.scene") << R"({"name":"Mine","rootGameObjects":[]})";
    std::ofstream(_root / ".gitignore") << "build/\n";

    // Adopting with the folder's old namespace keeps every asset UUID it had
    const Math::UUID legacy = IO::ResourceUUID::NamespaceForProjectDir(_root);
    const auto created = IO::CreateProject(_root, {.projectId = legacy});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->projectId, legacy);

    EXPECT_EQ(json::parse(ReadFile(_root / "assets" / "scenes" / "Main.scene")).at("name"), "Mine");
    EXPECT_EQ(ReadFile(_root / ".gitignore"), "build/\n");
    EXPECT_TRUE(fs::exists(_root / "assets" / "scripts" / "Example.lua"));
}

TEST_F(ProjectFileTest, CreateProjectRefusesBadOptionsWithoutWritingAnything)
{
    const fs::path dir = _root / "bad";
    const auto zeroId = IO::CreateProject(dir, {.projectId = Math::UUID::ZERO});
    ASSERT_FALSE(zeroId);
    EXPECT_EQ(zeroId.error().kind, IO::CreateProjectErrorKind::Failed);
    EXPECT_FALSE(fs::exists(dir)) << "options are checked before any folder is made";

    const auto blankName = IO::CreateProject(dir, {.name = "   "});
    ASSERT_FALSE(blankName);
    EXPECT_FALSE(fs::exists(dir));

    const auto badVersion = IO::CreateProject(dir, {.engineVersion = "one"});
    ASSERT_FALSE(badVersion);
    EXPECT_FALSE(fs::exists(dir));
}

TEST_F(ProjectFileTest, CreateProjectRefusesAFile)
{
    std::ofstream(_root / "file.txt") << "x";
    const auto created = IO::CreateProject(_root / "file.txt");
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().kind, IO::CreateProjectErrorKind::Failed);
}
