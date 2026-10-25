#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Application.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/physics/DeterminismLevel.hpp>
#include <engine/physics/PhysicsBackendInfo.hpp>
#include <engine/physics/PhysicsBackendKind.hpp>
#include <engine/physics/PhysicsFeatures.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The physics backend picker (J9, protocol 1.20.0): GetPhysicsBackends and SetPhysicsBackend through
// EditorServer::ExecuteCommand against a real project folder. Which backends the host build contains decides what
// can be set, so the tests that need a backend in the build skip in a build without one and the CI legs with PhysX or
// Jolt run them.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t BackendsType = static_cast<uint8_t>(ResponseType::PhysicsBackends);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> Backend(const std::string &name)
    {
        BufferWriter w;
        w.WriteString(name);
        return w.Release();
    }

    std::vector<uint8_t> JsonPayload(const json &value)
    {
        BufferWriter w;
        WriteJson(w, value);
        return w.Release();
    }

    /// PhysicsBackendsInfo of a PhysicsBackends frame (the test fails for any other frame)
    json Info(const Frame &frame)
    {
        EXPECT_EQ(frame.type, BackendsType) << frame.Text();
        if (frame.type != BackendsType)
            return json::object();
        BufferReader r(frame.payload);
        json info = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after PhysicsBackends";
        return info;
    }

    json Entry(const json &info, const std::string &id)
    {
        for (const json &entry : info.at("backends"))
        {
            if (entry.at("id") == id)
                return entry;
        }
        ADD_FAILURE() << "no backend " << id;
        return json::object();
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
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

    std::vector<Physics::PhysicsBackendKind> CompiledIn()
    {
        std::vector<Physics::PhysicsBackendKind> kinds;
        for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
        {
            if (Physics::IsBackendCompiledIn(kind))
                kinds.push_back(kind);
        }
        return kinds;
    }

    std::string Id(const Physics::PhysicsBackendKind kind)
    {
        return std::string(Physics::ToString(kind));
    }

    class EditorPhysicsBackendTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-physics-backend-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Physics Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Physics Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            // No backend runs in this process, so "active" is empty whatever an earlier test left
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            server.SetProject(_root, _project);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] std::string ProjectText() const { return ReadFile(IO::ProjectFile::PathIn(_root)); }

        json Get() { return Info(Execute(server, CommandType::GetPhysicsBackends)); }

        Frame Set(const std::string &name) { return Execute(server, CommandType::SetPhysicsBackend, Backend(name)); }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== The list ====================

TEST_F(EditorPhysicsBackendTest, ListsBothBackendsWithWhatTheBuildDeclares)
{
    const json info = Get();
    ASSERT_EQ(info.at("backends").size(), 2u);
    EXPECT_EQ(info.at("backends")[0].at("id"), "physx");
    EXPECT_EQ(info.at("backends")[1].at("id"), "jolt");
    EXPECT_EQ(info.at("backends")[0].at("displayName"), "PhysX");
    EXPECT_EQ(info.at("backends")[1].at("displayName"), "Jolt");

    for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
    {
        const Physics::PhysicsBackendDescription expected = Physics::DescribePhysicsBackend(kind);
        const json entry = Entry(info, Id(kind));
        EXPECT_EQ(entry.at("compiledIn"), Physics::IsBackendCompiledIn(kind)) << Id(kind);
        EXPECT_EQ(entry.at("determinism"), std::string(Physics::ToString(expected.determinism))) << Id(kind);
        EXPECT_EQ(entry.at("determinismDefault"), std::string(Physics::ToString(expected.determinismDefault))) << Id(kind);
        EXPECT_EQ(entry.at("supportsSnapshot"), expected.supportsSnapshot) << Id(kind);
        EXPECT_EQ(entry.at("supportsSetState"), expected.supportsSetState) << Id(kind);
        EXPECT_EQ(entry.at("deterministicSelectable"), expected.DeterministicSelectable()) << Id(kind);

        // features and missingFeatures split the four groups between them
        std::vector<std::string> all;
        for (const json &feature : entry.at("features"))
            all.push_back(feature.get<std::string>());
        for (const json &feature : entry.at("missingFeatures"))
            all.push_back(feature.get<std::string>());
        std::sort(all.begin(), all.end());
        EXPECT_EQ(all, (std::vector<std::string>{"events", "layers", "queries", "triggers"})) << Id(kind);
        EXPECT_EQ(entry.at("features").size() == 4u, expected.features.IsComplete()) << Id(kind);
    }
}

TEST_F(EditorPhysicsBackendTest, ABackendTheBuildLacksIsListedWithNoDeclarationsAndANote)
{
    for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
    {
        if (Physics::IsBackendCompiledIn(kind))
            continue;
        const json entry = Entry(Get(), Id(kind));
        EXPECT_FALSE(entry.at("compiledIn"));
        EXPECT_EQ(entry.at("determinism"), "None");
        EXPECT_EQ(entry.at("features").size(), 0u);
        EXPECT_FALSE(entry.at("deterministicSelectable")) << "never selectable without the backend";
        ASSERT_TRUE(entry.contains("note"));
        EXPECT_NE(entry.at("note").get<std::string>().find("Not in this host's build"), std::string::npos);
    }
}

TEST_F(EditorPhysicsBackendTest, ReportsTheProjectsSettingAndPreset)
{
    json info = Get();
    EXPECT_EQ(info.at("current"), "physx") << "the default when the project does not say";
    EXPECT_FALSE(info.at("configured"));
    EXPECT_EQ(info.at("active"), "");
    EXPECT_FALSE(info.at("restartRequired"));
    EXPECT_EQ(info.at("networkPreset"), "none");

    // The setting and the preset as the project file says them (SetProjectSettings is the existing write path)
    const Frame saved = Execute(server, CommandType::SetProjectSettings,
                                JsonPayload({{"physics", {{"backend", "jolt"}}}, {"network", {{"preset", "authoritative"}}}}));
    ASSERT_EQ(saved.type, static_cast<uint8_t>(ResponseType::ProjectInfo)) << saved.Text();
    info = Get();
    EXPECT_EQ(info.at("current"), "jolt");
    EXPECT_TRUE(info.at("configured"));
    EXPECT_EQ(info.at("networkPreset"), "authoritative");
}

TEST(EditorPhysicsBackendNoProjectTest, TheListNeedsNoProjectAndTheSetterDoes)
{
    EditorServer server;
    ASSERT_FALSE(server.HasProject());
    const json info = Info(Execute(server, CommandType::GetPhysicsBackends));
    EXPECT_EQ(info.at("current"), "physx");
    EXPECT_FALSE(info.at("configured"));
    EXPECT_EQ(info.at("networkPreset"), "none");
    EXPECT_EQ(info.at("backends").size(), 2u);

    const Frame refused = Execute(server, CommandType::SetPhysicsBackend, Backend("physx"));
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("No project"), std::string::npos) << refused.Text();
}

TEST(EditorPhysicsBackendPureTest, DeterministicNeedsCrossPlatformAndEveryGroup)
{
    using Physics::DeterminismLevel;
    using Physics::PhysicsFeature;
    using Physics::PhysicsFeatureSet;
    const PhysicsFeatureSet partial = PhysicsFeatureSet::None().With(PhysicsFeature::Layers).With(PhysicsFeature::Queries);
    for (const DeterminismLevel level :
         {DeterminismLevel::None, DeterminismLevel::SameBinary, DeterminismLevel::CrossPlatform})
    {
        EXPECT_EQ(Physics::DeterministicSelectable(level, PhysicsFeatureSet::All()), level == DeterminismLevel::CrossPlatform)
            << Physics::ToString(level);
        EXPECT_FALSE(Physics::DeterministicSelectable(level, partial)) << Physics::ToString(level);
        EXPECT_FALSE(Physics::DeterministicSelectable(level, PhysicsFeatureSet::None())) << Physics::ToString(level);
    }

    // A description that is not compiled in is never selectable, whatever it says
    Physics::PhysicsBackendDescription description;
    description.compiledIn = false;
    description.determinism = DeterminismLevel::CrossPlatform;
    description.features = PhysicsFeatureSet::All();
    EXPECT_FALSE(description.DeterministicSelectable());
    description.compiledIn = true;
    EXPECT_TRUE(description.DeterministicSelectable());

    EXPECT_EQ(Physics::FeatureId(PhysicsFeature::Layers), "layers");
    EXPECT_EQ(Physics::FeatureId(PhysicsFeature::Triggers), "triggers");
    EXPECT_EQ(Physics::FeatureId(PhysicsFeature::Events), "events");
    EXPECT_EQ(Physics::FeatureId(PhysicsFeature::Queries), "queries");
}

// ==================== Refusals ====================

TEST_F(EditorPhysicsBackendTest, AnUnknownBackendIsRefusedAndNothingIsSaved)
{
    const std::string before = ProjectText();
    for (const std::string name : {"havok", "", "phys x", "physx2", "jolt "})
    {
        const Frame refused = Set(name);
        EXPECT_EQ(refused.type, ErrorType) << '"' << name << '"';
        EXPECT_NE(refused.Text().find("Unknown physics backend"), std::string::npos) << refused.Text();
    }
    EXPECT_EQ(ProjectText(), before);
    EXPECT_TRUE(EventsOfKind(server, "projectChanged").empty());
}

TEST_F(EditorPhysicsBackendTest, ABackendNotCompiledIntoTheHostIsRefusedAndNothingIsSaved)
{
    const std::string before = ProjectText();
    bool any = false;
    for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
    {
        if (Physics::IsBackendCompiledIn(kind))
            continue;
        any = true;
        const Frame refused = Set(Id(kind));
        EXPECT_EQ(refused.type, ErrorType) << Id(kind);
        EXPECT_NE(refused.Text().find("has no"), std::string::npos) << refused.Text();
        EXPECT_NE(refused.Text().find(std::string(Physics::DisplayName(kind))), std::string::npos) << refused.Text();
    }
    if (!any)
        GTEST_SKIP() << "this build contains both backends";
    EXPECT_EQ(ProjectText(), before);
    EXPECT_TRUE(EventsOfKind(server, "projectChanged").empty());
}

// ==================== Setting it ====================

TEST_F(EditorPhysicsBackendTest, ABackendInTheBuildIsSavedInLowerCaseAndKeepsTheOtherSettings)
{
    const std::vector<Physics::PhysicsBackendKind> kinds = CompiledIn();
    if (kinds.empty())
        GTEST_SKIP() << "this build contains no physics backend";

    // The other physics settings and an unknown block survive the merge
    ASSERT_EQ(Execute(server, CommandType::SetProjectSettings,
                      JsonPayload({{"physics", {{"fixedTimestep", 0.01}}}, {"future", {{"keep", true}}}}))
                  .type,
              static_cast<uint8_t>(ResponseType::ProjectInfo));

    for (const auto kind : kinds)
    {
        std::string upper = Id(kind);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const json info = Info(Set(upper));
        EXPECT_EQ(info.at("current"), Id(kind));
        EXPECT_TRUE(info.at("configured"));
        EXPECT_EQ(info.at("active"), "") << "no backend runs in this process";
        EXPECT_FALSE(info.at("restartRequired")) << "restartRequired is only set against a running backend";

        const auto saved = IO::ProjectFile::Load(_root);
        ASSERT_TRUE(saved) << saved.error();
        EXPECT_EQ(saved->settings.at("physics").at("backend"), Id(kind)) << "saved in lower case";
        EXPECT_EQ(saved->settings.at("physics").at("fixedTimestep").get<double>(), 0.01);
        EXPECT_EQ(saved->settings.at("future").at("keep"), true);
    }
    EXPECT_FALSE(EventsOfKind(server, "projectChanged").empty());
    EXPECT_EQ(Get().at("current"), Id(kinds.back()));
}

TEST_F(EditorPhysicsBackendTest, TheDeterministicPresetRefusesABackendThatDoesNotDeclareIt)
{
    const std::vector<Physics::PhysicsBackendKind> kinds = CompiledIn();
    if (kinds.empty())
        GTEST_SKIP() << "this build contains no physics backend";
    ASSERT_EQ(Execute(server, CommandType::SetProjectSettings, JsonPayload({{"network", {{"preset", "deterministic"}}}})).type,
              static_cast<uint8_t>(ResponseType::ProjectInfo));
    EXPECT_EQ(Get().at("networkPreset"), "deterministic");

    for (const auto kind : kinds)
    {
        const std::string before = ProjectText();
        const bool selectable = Physics::DescribePhysicsBackend(kind).DeterministicSelectable();
        EXPECT_EQ(Entry(Get(), Id(kind)).at("deterministicSelectable"), selectable) << Id(kind);
        const Frame frame = Set(Id(kind));
        if (selectable)
        {
            EXPECT_EQ(Info(frame).at("current"), Id(kind));
        }
        else
        {
            EXPECT_EQ(frame.type, ErrorType) << Id(kind);
            EXPECT_NE(frame.Text().find("deterministic"), std::string::npos) << frame.Text();
            EXPECT_EQ(ProjectText(), before) << "a refused change saves nothing";
        }
    }
}

// ==================== Who may send them ====================

TEST_F(EditorPhysicsBackendTest, ViewersAreRefusedTheSetterAndMayReadTheList)
{
    const auto id = [](CommandType type) { return static_cast<uint8_t>(type); };
    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::GetPhysicsBackends)), EditorServer::CommandClass::Read);
    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::SetPhysicsBackend)), EditorServer::CommandClass::Write);
    for (const TokenRole role : {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin})
        EXPECT_TRUE(EditorServer::RoleAllows(role, id(CommandType::GetPhysicsBackends))) << TokenRoleName(role);
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id(CommandType::SetPhysicsBackend)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id(CommandType::SetPhysicsBackend)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id(CommandType::SetPhysicsBackend)));
    EXPECT_NE(EditorServer::RoleDeniedMessage(TokenRole::Viewer, id(CommandType::SetPhysicsBackend)).find("forbidden"),
              std::string::npos);
}

TEST_F(EditorPhysicsBackendTest, AReadOnlyHostAnswersTheListAndRefusesTheSetter)
{
    ASSERT_TRUE(server.SetReadOnly(true));
    const std::string before = ProjectText();
    EXPECT_EQ(Execute(server, CommandType::GetPhysicsBackends).type, BackendsType);
    for (const auto kind : {Physics::PhysicsBackendKind::PhysX, Physics::PhysicsBackendKind::Jolt})
        EXPECT_EQ(Set(Id(kind)).type, ErrorType) << Id(kind);
    EXPECT_EQ(ProjectText(), before);
}

TEST_F(EditorPhysicsBackendTest, TheSetterWritesTheProjectSoAPlayHostRefusesItAndTheListIsAllowed)
{
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::SetPhysicsBackend)));
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::GetPhysicsBackends)));
}

TEST_F(EditorPhysicsBackendTest, HelloAdvertisesPhysicsBackends)
{
    const auto capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "physicsBackends"), capabilities.end());
}
