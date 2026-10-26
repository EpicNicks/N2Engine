#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/NetworkPanel.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Time.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/network/NetworkObject.hpp>
#include <engine/network/NetworkPresets.hpp>
#include <engine/network/NetworkSettings.hpp>
#include <engine/physics/DeterminismLevel.hpp>
#include <engine/physics/PhysicsBackendInfo.hpp>
#include <engine/physics/PhysicsBackendKind.hpp>
#include <engine/physics/PhysicsFeatures.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;
namespace Panel = N2Engine::Editor::NetworkPanel;

// The editor's Network panel (B11, protocol 1.21.0): GetNetworkSettings, SetNetworkSettings and GetNetworkValidation.
// The report and the settings are pure functions of the project's settings and of what the host build offers
// (NetworkPanel::HostBuild), so the first half of this file hands them a build by hand and reaches every requirement in
// any build; the second half goes through EditorServer::ExecuteCommand against a real project folder, with the build the
// tests were compiled as.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t SettingsType = static_cast<uint8_t>(ResponseType::NetworkSettings);
    constexpr uint8_t ValidationType = static_cast<uint8_t>(ResponseType::NetworkValidation);

    // ---------------------------------------------------------------- builds

    Physics::PhysicsBackendDescription PhysX()
    {
        Physics::PhysicsBackendDescription d;
        d.kind = Physics::PhysicsBackendKind::PhysX;
        d.compiledIn = true;
        d.determinism = Physics::DeterminismLevel::SameBinary; // with physics.deterministic on
        d.determinismDefault = Physics::DeterminismLevel::None;
        d.features = Physics::PhysicsFeatureSet::All();
        d.supportsSnapshot = false;
        d.supportsSetState = true;
        return d;
    }

    Physics::PhysicsBackendDescription Jolt()
    {
        Physics::PhysicsBackendDescription d;
        d.kind = Physics::PhysicsBackendKind::Jolt;
        d.compiledIn = true;
        d.determinism = Physics::DeterminismLevel::CrossPlatform;
        d.determinismDefault = Physics::DeterminismLevel::SameBinary;
        d.features = Physics::PhysicsFeatureSet::All();
        d.supportsSnapshot = true;
        d.supportsSetState = true;
        return d;
    }

    Physics::PhysicsBackendDescription Absent(const Physics::PhysicsBackendKind kind)
    {
        Physics::PhysicsBackendDescription d;
        d.kind = kind;
        d.compiledIn = false;
        return d;
    }

    /// PhysX only, the strict profile off: what a plain build offers
    Panel::HostBuild PlainBuild()
    {
        Panel::HostBuild host;
        host.strictFp = false;
        host.contractionFree = true;
        host.available = {.physx = true, .jolt = false};
        host.backends[0] = PhysX();
        host.backends[1] = Absent(Physics::PhysicsBackendKind::Jolt);
        return host;
    }

    /// Both backends, the strict profile on: a build that can run the Deterministic preset
    Panel::HostBuild FullBuild()
    {
        Panel::HostBuild host;
        host.strictFp = true;
        host.contractionFree = true;
        host.available = {.physx = true, .jolt = true};
        host.backends[0] = PhysX();
        host.backends[1] = Jolt();
        return host;
    }

    /// Settings that meet every requirement of the Deterministic preset in FullBuild
    json DeterministicSettings()
    {
        return json{{"network", {{"preset", "deterministic"}}},
                    {"physics", {{"backend", "jolt"}, {"deterministic", true}, {"gpu", "off"}, {"fixedTimestep", 1.0 / 60.0}}},
                    {"math", {{"simdTier", "scalar"}}},
                    {"determinism", {{"luaSeed", 1234}}}};
    }

    json Report(const json &settings, const Network::NetworkPreset preset, const Panel::HostBuild &host)
    {
        return Panel::BuildValidationReport(settings, preset, host);
    }

    const json &Requirement(const json &report, const std::string &id)
    {
        static const json none = json::object();
        for (const json &requirement : report.at("requirements"))
        {
            if (requirement.at("id") == id)
                return requirement;
        }
        ADD_FAILURE() << "no requirement " << id;
        return none;
    }

    std::vector<std::string> UnmetIds(const json &report)
    {
        std::vector<std::string> ids;
        for (const json &requirement : report.at("requirements"))
        {
            if (!requirement.at("met").get<bool>())
                ids.push_back(requirement.at("id").get<std::string>());
        }
        return ids;
    }

    const json &PresetEntry(const json &info, const std::string &id)
    {
        static const json none = json::object();
        for (const json &preset : info.at("presets"))
        {
            if (preset.at("id") == id)
                return preset;
        }
        ADD_FAILURE() << "no preset " << id;
        return none;
    }

    const json &RowOf(const json &info, const std::string &preset, const std::string &key)
    {
        static const json none = json::object();
        for (const json &row : PresetEntry(info, preset).at("rows"))
        {
            // By the row's key, or by the setting it carries (the row "state.sync" carries network.authoritative.snapshotRate)
            if (row.at("key") == key || (row.contains("settingKey") && row.at("settingKey") == key))
                return row;
        }
        ADD_FAILURE() << "no row " << key << " in " << preset;
        return none;
    }

    // ---------------------------------------------------------------- protocol helpers

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

    std::vector<uint8_t> StringPayload(const std::string &value)
    {
        BufferWriter w;
        w.WriteString(value);
        return w.Release();
    }

    std::vector<uint8_t> SetPayload(const std::string &preset, const json &settings)
    {
        BufferWriter w;
        w.WriteString(preset);
        WriteJson(w, settings);
        return w.Release();
    }

    std::vector<uint8_t> JsonPayload(const json &value)
    {
        BufferWriter w;
        WriteJson(w, value);
        return w.Release();
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    json JsonOf(const Frame &frame, const uint8_t expected)
    {
        EXPECT_EQ(frame.type, expected) << frame.Text();
        if (frame.type != expected)
            return json::object();
        BufferReader r(frame.payload);
        json value = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the JSON value";
        return value;
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::size_t ProjectChangedEvents(const EditorServer &server)
    {
        std::size_t count = 0;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            if (event.value("kind", "") == "projectChanged")
                ++count;
        }
        return count;
    }

    bool AnyBackendSelectable()
    {
        const Panel::HostBuild host = Panel::CurrentHostBuild();
        return host.Backend(Physics::PhysicsBackendKind::PhysX).DeterministicSelectable() ||
               host.Backend(Physics::PhysicsBackendKind::Jolt).DeterministicSelectable();
    }

    class EditorNetworkPanelTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-network-panel-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Net Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Net Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
        }

        void TearDown() override
        {
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            // SetProjectSettings applies physics.fixedTimestep to the running engine; put the default back
            (void)Time::SetFixedTimestep(0.02);
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] std::string ProjectText() const { return ReadFile(IO::ProjectFile::PathIn(_root)); }

        json Get() { return JsonOf(Execute(server, CommandType::GetNetworkSettings), SettingsType); }

        Frame Set(const std::string &preset, const json &settings)
        {
            return Execute(server, CommandType::SetNetworkSettings, SetPayload(preset, settings));
        }

        json Validate(const std::string &preset = "")
        {
            return JsonOf(Execute(server, CommandType::GetNetworkValidation, StringPayload(preset)), ValidationType);
        }

        [[nodiscard]] json SavedSettings() const
        {
            const auto saved = IO::ProjectFile::Load(_root);
            EXPECT_TRUE(saved) << (saved ? "" : saved.error());
            return saved ? saved->settings : json::object();
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== The settings and the three preset bundles ====================

TEST(NetworkPanelSettingsTest, AProjectWithoutTheBlockIsPresetNoneWithThreePresetsInOrder)
{
    const json info = Panel::BuildSettingsInfo(json::object(), PlainBuild(), {});
    EXPECT_EQ(info.at("preset"), "none");
    EXPECT_FALSE(info.at("customised"));
    EXPECT_FALSE(info.at("storedCustomised"));
    EXPECT_EQ(info.at("tickRate"), 60);
    EXPECT_EQ(info.at("values").at("transport").at("port"), 7777);
    EXPECT_EQ(info.at("values").at("deterministic").at("mode"), "lockstep");
    EXPECT_EQ(info.at("lostGuarantees").size(), 0u);

    ASSERT_EQ(info.at("presets").size(), 3u);
    EXPECT_EQ(info.at("presets")[0].at("id"), "none");
    EXPECT_EQ(info.at("presets")[1].at("id"), "authoritative");
    EXPECT_EQ(info.at("presets")[2].at("id"), "deterministic");
    EXPECT_EQ(info.at("presets")[1].at("label"), "Server authoritative");
    EXPECT_TRUE(PresetEntry(info, "none").at("selected").get<bool>());
    EXPECT_FALSE(PresetEntry(info, "deterministic").at("selected").get<bool>());

    // Every preset lists every row of the engine's table, in its order
    const auto &table = Network::PresetTable();
    for (const json &preset : info.at("presets"))
    {
        ASSERT_EQ(preset.at("rows").size(), table.size()) << preset.at("id");
        for (std::size_t i = 0; i < table.size(); ++i)
            EXPECT_EQ(preset.at("rows")[i].at("key"), table[i].key);
    }
    EXPECT_EQ(info.at("problems").size(), 0u);
}

TEST(NetworkPanelSettingsTest, RowsSayWhetherThePresetLocksOrLeavesASettingTunableAndWhereItComesFrom)
{
    const json info = Panel::BuildSettingsInfo(json::object(), FullBuild(), {});

    // Deterministic locks the GPU setting and checks it at startup; the project does not set it
    const json &gpu = RowOf(info, "deterministic", "physics.gpu");
    EXPECT_TRUE(gpu.at("governed"));
    EXPECT_TRUE(gpu.at("locked"));
    EXPECT_FALSE(gpu.at("tunable"));
    EXPECT_TRUE(gpu.at("startupChecked"));
    EXPECT_EQ(gpu.at("settingKey"), "physics.gpu");
    EXPECT_EQ(gpu.at("value"), "off");
    EXPECT_EQ(gpu.at("current"), "off");
    EXPECT_TRUE(gpu.at("atPresetValue"));
    EXPECT_EQ(gpu.at("source"), "default");
    EXPECT_FALSE(gpu.at("lostGuarantee").get<std::string>().empty());

    // Server authoritative only recommends it: tunable, not locked, not checked at startup
    const json &authoritativeGpu = RowOf(info, "authoritative", "physics.gpu");
    EXPECT_TRUE(authoritativeGpu.at("tunable"));
    EXPECT_FALSE(authoritativeGpu.at("locked"));
    EXPECT_FALSE(authoritativeGpu.at("startupChecked"));

    // None governs nothing
    const json &noneGpu = RowOf(info, "none", "physics.gpu");
    EXPECT_FALSE(noneGpu.at("governed"));
    EXPECT_FALSE(noneGpu.at("locked"));
    EXPECT_FALSE(noneGpu.at("tunable"));

    // A requirement on the build has no setting: its source is the build
    const json &strict = RowOf(info, "deterministic", "build.strictFp");
    EXPECT_TRUE(strict.at("locked"));
    EXPECT_EQ(strict.at("source"), "build");
    EXPECT_FALSE(strict.contains("settingKey"));
    EXPECT_FALSE(strict.contains("current"));

    // The fixed timestep follows the tick rate, in seconds
    EXPECT_NEAR(RowOf(info, "deterministic", "physics.fixedTimestep").at("value").get<double>(), 1.0 / 60.0, 1e-9);
    const json thirty = Panel::BuildSettingsInfo(json{{"network", {{"tickRate", 30}}}}, FullBuild(), {});
    EXPECT_NEAR(RowOf(thirty, "deterministic", "physics.fixedTimestep").at("value").get<double>(), 1.0 / 30.0, 1e-9);

    // Tunable rows carry the preset's default and the range of the setting
    const json &snapshot = RowOf(info, "authoritative", "network.authoritative.snapshotRate");
    EXPECT_TRUE(snapshot.at("tunable"));
    EXPECT_EQ(snapshot.at("value"), 30);
    bool foundRange = false;
    for (const json &range : info.at("ranges"))
    {
        if (range.at("key") == "network.tickRate")
        {
            foundRange = true;
            EXPECT_EQ(range.at("min"), 30);
            EXPECT_EQ(range.at("max"), 120);
            EXPECT_EQ(range.at("default"), 60);
            EXPECT_TRUE(range.at("integer"));
        }
    }
    EXPECT_TRUE(foundRange);
}

TEST(NetworkPanelSettingsTest, TheSourceIsTheProjectTheOverridesOrTheDefault)
{
    const json settings = {{"network", {{"preset", "authoritative"},
                                        {"authoritative", {{"snapshotRate", 20}}},
                                        {"overrides", {{"physics.gpu", "auto"}}}}},
                           {"physics", {{"fixedTimestep", 0.02}}}};
    const json info = Panel::BuildSettingsInfo(settings, FullBuild(), {});
    EXPECT_EQ(info.at("preset"), "authoritative");
    EXPECT_EQ(RowOf(info, "authoritative", "network.authoritative.snapshotRate").at("source"), "project");
    EXPECT_EQ(RowOf(info, "authoritative", "network.authoritative.snapshotRate").at("current"), 20);
    EXPECT_FALSE(RowOf(info, "authoritative", "network.authoritative.snapshotRate").at("atPresetValue").get<bool>());
    EXPECT_EQ(RowOf(info, "authoritative", "network.authoritative.smoothingMs").at("source"), "default");
    EXPECT_EQ(RowOf(info, "authoritative", "physics.gpu").at("source"), "override");
    EXPECT_EQ(RowOf(info, "authoritative", "physics.fixedTimestep").at("source"), "project");
    EXPECT_NEAR(RowOf(info, "authoritative", "physics.fixedTimestep").at("current").get<double>(), 0.02, 1e-12);
}

TEST(NetworkPanelSettingsTest, ChangingALockedSettingMakesThePresetCustomisedAndNamesWhatIsLost)
{
    json settings = DeterministicSettings();
    json info = Panel::BuildSettingsInfo(settings, FullBuild(), {});
    EXPECT_FALSE(info.at("customised")) << "every explicit locked value is the preset's";
    EXPECT_EQ(info.at("lostGuarantees").size(), 0u);

    settings["physics"]["gpu"] = "auto";
    settings["math"]["simdTier"] = "avx";
    info = Panel::BuildSettingsInfo(settings, FullBuild(), {});
    EXPECT_TRUE(info.at("customised"));
    std::vector<std::string> keys;
    for (const json &lost : info.at("lostGuarantees"))
    {
        keys.push_back(lost.at("settingKey").get<std::string>());
        EXPECT_FALSE(lost.at("text").get<std::string>().empty());
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{"math.simdTier", "physics.gpu"}));
    EXPECT_FALSE(RowOf(info, "deterministic", "physics.gpu").at("atPresetValue").get<bool>());

    // A tunable setting changed loses nothing
    json tunable = DeterministicSettings();
    tunable["network"]["tickRate"] = 30;
    tunable["physics"]["fixedTimestep"] = 1.0 / 30.0;
    tunable["network"]["deterministic"] = {{"inputDelay", 4}};
    EXPECT_FALSE(Panel::BuildSettingsInfo(tunable, FullBuild(), {}).at("customised"));

    // A project that does not set the locked setting is not customised, even when the requirement is unmet
    EXPECT_FALSE(Panel::BuildSettingsInfo(json{{"network", {{"preset", "deterministic"}}}}, PlainBuild(), {})
                     .at("customised"));

    // The stored flag is passed on as written
    EXPECT_TRUE(Panel::BuildSettingsInfo(json{{"network", {{"customised", true}}}}, PlainBuild(), {})
                    .at("storedCustomised"));
}

TEST(NetworkPanelSettingsTest, ABadStoredValueIsReportedAsAProblemAndDefaulted)
{
    const json info = Panel::BuildSettingsInfo(json{{"network", {{"preset", "weird"}, {"tickRate", 600}}}}, PlainBuild(), {});
    EXPECT_EQ(info.at("preset"), "none");
    EXPECT_EQ(info.at("tickRate"), 120) << "clamped to its range by the engine's reader";
    EXPECT_EQ(info.at("problems").size(), 2u);
}

// ==================== Deterministic shown disabled with the reason ====================

TEST(NetworkPanelSettingsTest, DeterministicIsDisabledWithTheReasonWhenNoBackendInTheBuildCanCarryIt)
{
    const json info = Panel::BuildSettingsInfo(json::object(), PlainBuild(), {});
    EXPECT_TRUE(PresetEntry(info, "none").at("selectable"));
    EXPECT_TRUE(PresetEntry(info, "authoritative").at("selectable"));
    const json &deterministic = PresetEntry(info, "deterministic");
    EXPECT_FALSE(deterministic.at("selectable"));
    const std::string reason = deterministic.at("unavailableReason").get<std::string>();
    EXPECT_NE(reason.find("Requires a deterministic physics backend (none in this build)"), std::string::npos) << reason;
    EXPECT_NE(reason.find("physx"), std::string::npos) << "names the backend and what it declares: " << reason;
    EXPECT_FALSE(PresetEntry(info, "none").contains("unavailableReason"));
    EXPECT_EQ(info.at("backend").at("selected"), "physx");
    EXPECT_EQ(info.at("backend").at("deterministicSelectable").size(), 0u);
}

TEST(NetworkPanelSettingsTest, DeterministicIsDisabledUntilTheProjectsBackendIsOneThatQualifies)
{
    // The build has Jolt, but the project still runs on PhysX (the default): the reason says which backend to choose
    json info = Panel::BuildSettingsInfo(json::object(), FullBuild(), {});
    const json &deterministic = PresetEntry(info, "deterministic");
    EXPECT_FALSE(deterministic.at("selectable"));
    const std::string reason = deterministic.at("unavailableReason").get<std::string>();
    EXPECT_NE(reason.find("Choose jolt for physics.backend first"), std::string::npos) << reason;
    EXPECT_EQ(info.at("backend").at("deterministicSelectable"), json::array({"jolt"}));

    // Jolt without physics.deterministic declares only SameBinary: still not selectable, and the report says why
    info = Panel::BuildSettingsInfo(json{{"physics", {{"backend", "jolt"}}}}, FullBuild(), {});
    EXPECT_FALSE(PresetEntry(info, "deterministic").at("selectable"));

    // Jolt with physics.deterministic on, in the full build: selectable, no reason
    info = Panel::BuildSettingsInfo(json{{"physics", {{"backend", "jolt"}, {"deterministic", true}}}}, FullBuild(), {});
    EXPECT_TRUE(PresetEntry(info, "deterministic").at("selectable"));
    EXPECT_FALSE(PresetEntry(info, "deterministic").contains("unavailableReason"));
    EXPECT_EQ(info.at("backend").at("selected"), "jolt");
}

TEST(NetworkPanelSettingsTest, AProjectAskingForABackendTheBuildLacksCannotChooseDeterministic)
{
    const json info = Panel::BuildSettingsInfo(json{{"physics", {{"backend", "jolt"}}}}, PlainBuild(), {});
    const json &deterministic = PresetEntry(info, "deterministic");
    EXPECT_FALSE(deterministic.at("selectable"));
    EXPECT_NE(deterministic.at("unavailableReason").get<std::string>().find("this build has no such backend"),
              std::string::npos)
        << deterministic.at("unavailableReason");
    // Without the Deterministic preset the engine falls back to the backend the build has (a warning at start-up)
    EXPECT_EQ(info.at("backend").at("selected"), "physx");
}

// ==================== The nudge ====================

TEST(NetworkPanelNudgeTest, AProjectThatLooksNetworkedButIsPresetNoneIsNudged)
{
    json info = Panel::BuildSettingsInfo(json::object(), PlainBuild(), {.networkObjects = 0});
    EXPECT_FALSE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("reasons").size(), 0u);
    EXPECT_EQ(info.at("nudge").at("hint"), "");

    info = Panel::BuildSettingsInfo(json::object(), PlainBuild(), {.networkObjects = 1});
    EXPECT_TRUE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("reasons"), json::array({"scene"}));
    EXPECT_EQ(info.at("nudge").at("networkObjects"), 1);
    EXPECT_NE(info.at("nudge").at("hint").get<std::string>().find("1 NetworkObject component but"), std::string::npos);

    info = Panel::BuildSettingsInfo(json::object(), PlainBuild(), {.networkObjects = 3});
    EXPECT_NE(info.at("nudge").at("hint").get<std::string>().find("3 NetworkObject components"), std::string::npos);
}

TEST(NetworkPanelNudgeTest, NetworkSettingsWithoutAPresetAlsoNudge)
{
    // Settings of the block other than the preset: the project was set up for a model it has not chosen
    json info = Panel::BuildSettingsInfo(json{{"network", {{"transport", {{"port", 9000}}}}}}, PlainBuild(), {});
    EXPECT_TRUE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("reasons"), json::array({"settings"}));
    EXPECT_FALSE(info.at("nudge").at("hint").get<std::string>().empty());

    // Both reasons together
    info = Panel::BuildSettingsInfo(json{{"network", {{"tickRate", 30}}}}, PlainBuild(), {.networkObjects = 2});
    EXPECT_EQ(info.at("nudge").at("reasons"), json::array({"scene", "settings"}));

    // Only the preset, set to none explicitly, is no sign of anything
    info = Panel::BuildSettingsInfo(json{{"network", {{"preset", "none"}}}}, PlainBuild(), {});
    EXPECT_FALSE(info.at("nudge").at("show"));
}

TEST(NetworkPanelNudgeTest, ARealPresetSilencesTheNudge)
{
    for (const char *preset : {"authoritative", "deterministic"})
    {
        const json info = Panel::BuildSettingsInfo(json{{"network", {{"preset", preset}, {"tickRate", 30}}}}, FullBuild(),
                                                   {.networkObjects = 5});
        EXPECT_FALSE(info.at("nudge").at("show")) << preset;
        EXPECT_EQ(info.at("nudge").at("networkObjects"), 5) << "still counted";
        EXPECT_EQ(info.at("nudge").at("hint"), "");
    }
}

// ==================== The validation report ====================

TEST(NetworkPanelReportTest, DeterministicOnAPlainBuildListsEveryRequirementWithTheReasonItIsUnmet)
{
    // The fixed timestep is set explicitly: its default (50 Hz) is not the 60 Hz tick rate, a check of its own below
    const json report = Report(json{{"physics", {{"fixedTimestep", 1.0 / 60.0}}}}, Network::NetworkPreset::Deterministic,
                               PlainBuild());
    EXPECT_EQ(report.at("preset"), "deterministic");
    EXPECT_EQ(report.at("projectPreset"), "none") << "the preset was asked for, not chosen";
    EXPECT_TRUE(report.at("refused"));
    EXPECT_EQ(report.at("warnings").size(), 0u);

    std::vector<std::string> ids;
    for (const json &requirement : report.at("requirements"))
    {
        ids.push_back(requirement.at("id").get<std::string>());
        EXPECT_EQ(requirement.at("met").get<bool>(), requirement.at("detail") == "") << requirement.at("id");
        EXPECT_FALSE(requirement.at("text").get<std::string>().empty());
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"physics-backend", "physics-features", "strict-fp", "simd-scalar",
                                             "lua-seed", "engine-trig", "physics-threads", "gpu-physics-off",
                                             "fixed-timestep", "tick-loop"}));

    EXPECT_EQ(UnmetIds(report), (std::vector<std::string>{"physics-backend", "strict-fp", "simd-scalar", "lua-seed",
                                                          "physics-threads"}));
    EXPECT_EQ(report.at("unmet"), 5);
    EXPECT_NE(Requirement(report, "physics-backend").at("detail").get<std::string>().find("physx"), std::string::npos);
    EXPECT_NE(Requirement(report, "physics-backend").at("detail").get<std::string>().find("None"), std::string::npos);
    EXPECT_NE(Requirement(report, "strict-fp").at("detail").get<std::string>().find("N2ENGINE_STRICT_FP"), std::string::npos);
    EXPECT_EQ(Requirement(report, "simd-scalar").at("detail"), "math.simdTier is auto");
    EXPECT_NE(Requirement(report, "lua-seed").at("detail").get<std::string>().find("determinism.luaSeed"), std::string::npos);
    EXPECT_NE(report.at("summary").get<std::string>().find("5 of 10 requirements are unmet"), std::string::npos)
        << report.at("summary");

    // What was checked
    const json &environment = report.at("environment");
    EXPECT_EQ(environment.at("backend"), "physx");
    EXPECT_EQ(environment.at("backendDeterminism"), "None");
    EXPECT_FALSE(environment.at("strictFp"));
    EXPECT_EQ(environment.at("simdTier"), "auto");
    EXPECT_FALSE(environment.at("luaSeedSet"));
    EXPECT_EQ(environment.at("physicsGpu"), "off");
    EXPECT_EQ(environment.at("tickRate"), 60);
}

TEST(NetworkPanelReportTest, AFullyConfiguredProjectMeetsEveryRequirementAndIsNotRefused)
{
    const json report = Report(DeterministicSettings(), Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_FALSE(report.at("refused"));
    EXPECT_EQ(report.at("unmet"), 0);
    EXPECT_EQ(UnmetIds(report), std::vector<std::string>{});
    EXPECT_EQ(report.at("requirements").size(), 10u);
    for (const json &requirement : report.at("requirements"))
    {
        EXPECT_EQ(requirement.at("detail"), "") << requirement.at("id");
        EXPECT_FALSE(requirement.contains("fix")) << "a met requirement has no fix";
    }
    EXPECT_EQ(report.at("projectPreset"), "deterministic");
    EXPECT_NE(report.at("summary").get<std::string>().find("all 10 requirements are met"), std::string::npos)
        << report.at("summary");
    EXPECT_EQ(report.at("environment").at("backend"), "jolt");
    EXPECT_EQ(report.at("environment").at("backendDeterminism"), "CrossPlatform");
}

TEST(NetworkPanelReportTest, RollbackModeAddsTheSnapshotRequirement)
{
    json settings = DeterministicSettings();
    settings["network"]["deterministic"] = {{"mode", "rollback"}};
    json report = Report(settings, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_EQ(report.at("requirements").size(), 11u);
    EXPECT_TRUE(Requirement(report, "snapshot").at("met"));
    EXPECT_FALSE(report.at("refused"));

    Panel::HostBuild noSnapshot = FullBuild();
    noSnapshot.backends[1].supportsSnapshot = false;
    report = Report(settings, Network::NetworkPreset::Deterministic, noSnapshot);
    EXPECT_EQ(UnmetIds(report), std::vector<std::string>{"snapshot"});
    EXPECT_TRUE(report.at("refused"));
    // Lockstep waives it
    EXPECT_EQ(Report(DeterministicSettings(), Network::NetworkPreset::Deterministic, noSnapshot).at("requirements").size(), 10u);
}

TEST(NetworkPanelReportTest, EachRequirementIsUnmetByItsOwnCauseAlone)
{
    struct Case
    {
        std::string id;
        void (*spoil)(json &, Panel::HostBuild &);
    };
    const std::vector<Case> cases = {
        {"physics-backend", [](json &s, Panel::HostBuild &) { s["physics"]["deterministic"] = false; }},
        {"physics-backend", [](json &s, Panel::HostBuild &) { s["physics"]["backend"] = "physx"; }},
        {"physics-features", [](json &, Panel::HostBuild &h)
         { h.backends[1].features = Physics::PhysicsFeatureSet::None().With(Physics::PhysicsFeature::Layers); }},
        {"strict-fp", [](json &, Panel::HostBuild &h) { h.strictFp = false; }},
        {"simd-scalar", [](json &s, Panel::HostBuild &) { s["math"]["simdTier"] = "avx"; }},
        {"simd-scalar", [](json &s, Panel::HostBuild &) { s["math"].erase("simdTier"); }},
        {"lua-seed", [](json &s, Panel::HostBuild &) { s.erase("determinism"); }},
        {"engine-trig", [](json &, Panel::HostBuild &h) { h.contractionFree = false; }},
        {"gpu-physics-off", [](json &s, Panel::HostBuild &) { s["physics"]["gpu"] = "auto"; }},
        {"gpu-physics-off", [](json &s, Panel::HostBuild &) { s["physics"]["gpu"] = "on"; }},
        {"fixed-timestep", [](json &s, Panel::HostBuild &) { s["physics"]["fixedTimestep"] = 0.02; }},
        {"fixed-timestep", [](json &s, Panel::HostBuild &) { s["network"]["tickRate"] = 120; }},
    };
    for (const Case &c : cases)
    {
        json settings = DeterministicSettings();
        Panel::HostBuild host = FullBuild();
        c.spoil(settings, host);
        const json report = Report(settings, Network::NetworkPreset::Deterministic, host);
        const std::vector<std::string> unmet = UnmetIds(report);
        // physics-threads is met by SameBinary, which Jolt declares without physics.deterministic as well
        EXPECT_EQ(unmet, std::vector<std::string>{c.id}) << c.id << " " << report.dump();
        EXPECT_TRUE(report.at("refused")) << c.id;
        EXPECT_FALSE(Requirement(report, c.id).at("detail").get<std::string>().empty()) << c.id;
    }

    // physics-threads: a backend that declares less than SameBinary (PhysX with physics.deterministic off)
    json settings = DeterministicSettings();
    settings["physics"]["backend"] = "physx";
    const json report = Report(settings, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_EQ(UnmetIds(report), std::vector<std::string>{"physics-backend"}) << "PhysX with physics.deterministic is SameBinary";
    settings["physics"]["deterministic"] = false;
    EXPECT_EQ(UnmetIds(Report(settings, Network::NetworkPreset::Deterministic, FullBuild())),
              (std::vector<std::string>{"physics-backend", "physics-threads"}));
}

TEST(NetworkPanelReportTest, NoPhysicsBackendInTheBuildIsReported)
{
    Panel::HostBuild none;
    none.strictFp = true;
    none.contractionFree = true;
    none.backends[0] = Absent(Physics::PhysicsBackendKind::PhysX);
    none.backends[1] = Absent(Physics::PhysicsBackendKind::Jolt);
    const json report = Report(DeterministicSettings(), Network::NetworkPreset::Deterministic, none);
    EXPECT_TRUE(report.at("refused"));
    EXPECT_NE(Requirement(report, "physics-backend").at("detail").get<std::string>().find("no such backend"), std::string::npos)
        << "the project asks for jolt: " << Requirement(report, "physics-backend").at("detail");
    EXPECT_EQ(report.at("environment").at("backend"), "");

    json unset = DeterministicSettings();
    unset["physics"].erase("backend");
    const json noBackend = Report(unset, Network::NetworkPreset::Deterministic, none);
    EXPECT_EQ(Requirement(noBackend, "physics-backend").at("detail"), "there is no physics backend");
}

TEST(NetworkPanelReportTest, MechanicalFixesComeAsSettingsPatchesAndMeetWhatTheyFix)
{
    // The build has Jolt; the project has nothing set. The lua seed has no fix, strict-fp and trig are the build's.
    json settings = json::object();
    const json report = Report(settings, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_FALSE(Requirement(report, "lua-seed").contains("fix"));
    EXPECT_EQ(Requirement(report, "simd-scalar").at("fix").at("settings"), (json{{"math", {{"simdTier", "scalar"}}}}));
    EXPECT_EQ(Requirement(report, "physics-backend").at("fix").at("settings"),
              (json{{"physics", {{"backend", "jolt"}, {"deterministic", true}}}}));
    EXPECT_FALSE(Requirement(report, "simd-scalar").at("fix").at("label").get<std::string>().empty());

    for (const json &requirement : report.at("requirements"))
    {
        if (requirement.contains("fix"))
            settings.merge_patch(requirement.at("fix").at("settings"));
    }
    json fixed = Report(settings, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_EQ(UnmetIds(fixed), std::vector<std::string>{"lua-seed"}) << fixed.dump();
    settings["determinism"] = {{"luaSeed", 7}};
    fixed = Report(settings, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_FALSE(fixed.at("refused")) << fixed.dump();

    // GPU and the timestep
    json gpu = json{{"physics", {{"gpu", "on"}, {"fixedTimestep", 0.02}}}, {"network", {{"tickRate", 30}}}};
    const json gpuReport = Report(gpu, Network::NetworkPreset::Deterministic, FullBuild());
    EXPECT_EQ(Requirement(gpuReport, "gpu-physics-off").at("fix").at("settings"), (json{{"physics", {{"gpu", "off"}}}}));
    EXPECT_NEAR(Requirement(gpuReport, "fixed-timestep").at("fix").at("settings").at("physics").at("fixedTimestep").get<double>(),
                1.0 / 30.0, 1e-9);

    // No backend of the build qualifies: no fix to offer
    const json plain = Report(json::object(), Network::NetworkPreset::Deterministic, PlainBuild());
    EXPECT_FALSE(Requirement(plain, "physics-backend").contains("fix"));
    // PhysX with physics.deterministic off: turning it on fixes the thread check (the backend one has no fix here)
    EXPECT_EQ(Requirement(plain, "physics-threads").at("fix").at("settings"), (json{{"physics", {{"deterministic", true}}}}));
    EXPECT_FALSE(Requirement(plain, "strict-fp").contains("fix"));
}

TEST(NetworkPanelReportTest, ServerAuthoritativeOnlyWarns)
{
    json report = Report(json::object(), Network::NetworkPreset::Authoritative, PlainBuild());
    EXPECT_FALSE(report.at("refused"));
    EXPECT_EQ(report.at("requirements").size(), 0u);
    EXPECT_EQ(report.at("warnings").size(), 0u);
    EXPECT_NE(report.at("summary").get<std::string>().find("no warnings"), std::string::npos);

    report = Report(json{{"physics", {{"gpu", "on"}}}}, Network::NetworkPreset::Authoritative, PlainBuild());
    ASSERT_EQ(report.at("warnings").size(), 1u);
    EXPECT_NE(report.at("warnings")[0].get<std::string>().find("physics.gpu is on"), std::string::npos);
    EXPECT_FALSE(report.at("refused"));
    EXPECT_NE(report.at("summary").get<std::string>().find("1 warning"), std::string::npos);

    Panel::HostBuild noSetState = PlainBuild();
    noSetState.backends[0].supportsSetState = false;
    report = Report(json::object(), Network::NetworkPreset::Authoritative, noSetState);
    ASSERT_EQ(report.at("warnings").size(), 1u);
    EXPECT_NE(report.at("warnings")[0].get<std::string>().find("set-state"), std::string::npos);

    Panel::HostBuild noBackend = PlainBuild();
    noBackend.available = {};
    noBackend.backends[0] = Absent(Physics::PhysicsBackendKind::PhysX);
    report = Report(json::object(), Network::NetworkPreset::Authoritative, noBackend);
    ASSERT_EQ(report.at("warnings").size(), 1u);
    EXPECT_NE(report.at("warnings")[0].get<std::string>().find("no physics backend"), std::string::npos);
}

TEST(NetworkPanelReportTest, NoneChecksNothing)
{
    const json report = Report(json{{"physics", {{"gpu", "on"}}}}, Network::NetworkPreset::None, PlainBuild());
    EXPECT_EQ(report.at("preset"), "none");
    EXPECT_FALSE(report.at("refused"));
    EXPECT_EQ(report.at("unmet"), 0);
    EXPECT_EQ(report.at("requirements").size(), 0u);
    EXPECT_EQ(report.at("warnings").size(), 0u);
    EXPECT_NE(report.at("summary").get<std::string>().find("nothing is checked"), std::string::npos);
}

// ==================== What a change may say ====================

TEST(NetworkPanelChangeTest, TheCommandIsAMergePatchOfTheNetworkBlock)
{
    auto patch = Panel::BuildChangePatch(json::object(), "Authoritative", json{{"tickRate", 30}, {"transport", {{"port", 9000}}}},
                                         PlainBuild());
    ASSERT_TRUE(patch) << patch.error();
    EXPECT_EQ(*patch, (json{{"network", {{"preset", "authoritative"}, {"tickRate", 30}, {"transport", {{"port", 9000}}}}}}));

    patch = Panel::BuildChangePatch(json::object(), "", json::object(), PlainBuild());
    ASSERT_TRUE(patch);
    EXPECT_EQ(*patch, (json{{"network", json::object()}})) << "nothing to change";

    // A null returns a setting, or a whole block, to its default
    patch = Panel::BuildChangePatch(json::object(), "", json{{"authoritative", nullptr}, {"tickRate", nullptr}}, PlainBuild());
    ASSERT_TRUE(patch);
    EXPECT_TRUE(patch->at("network").at("authoritative").is_null());
    EXPECT_TRUE(patch->at("network").at("tickRate").is_null());

    // Every tunable setting of the block is accepted
    const json everything = {{"tickRate", 120},
                             {"transport", {{"udp", false}, {"webSocket", false}, {"port", 1}}},
                             {"authoritative", {{"snapshotRate", 10}, {"interpolationDelayMs", 0}, {"inputRedundancy", 1},
                                                {"predictionMaxMs", 0}, {"smoothingMs", 0}, {"snapThreshold", 0.5},
                                                {"lagCompMaxMs", 0}}},
                             {"deterministic", {{"mode", "rollback"}, {"inputDelay", 0}, {"rollbackWindow", 15},
                                                {"hashInterval", 600}}}};
    patch = Panel::BuildChangePatch(json::object(), "", everything, PlainBuild());
    ASSERT_TRUE(patch) << patch.error();
    EXPECT_EQ(patch->at("network"), everything);
}

TEST(NetworkPanelChangeTest, RefusalsNameTheProblem)
{
    const Panel::HostBuild host = PlainBuild();
    const auto error = [&](const std::string &preset, const json &settings, const json &project = json::object())
    {
        const auto patch = Panel::BuildChangePatch(project, preset, settings, host);
        EXPECT_FALSE(patch) << settings.dump();
        return patch ? std::string{} : patch.error();
    };

    EXPECT_NE(error("lockstep", json::object()).find("preset must be"), std::string::npos);
    EXPECT_NE(error(" none", json::object()).find("preset must be"), std::string::npos);
    EXPECT_NE(error("", nullptr).find("must be a JSON object"), std::string::npos);
    EXPECT_NE(error("", json::array()).find("must be a JSON object"), std::string::npos);
    EXPECT_NE(error("", "tickRate").find("must be a JSON object"), std::string::npos);

    // The preset, the stored overrides and the flag are not for this field; nor are unknown or locked settings
    for (const char *key : {"preset", "overrides", "customised", "future", "physics", "transport.port", "tickrate"})
    {
        const std::string message = error("", json{{key, 1}});
        EXPECT_NE(message.find(key), std::string::npos) << message;
        EXPECT_NE(message.find("not a tunable network setting"), std::string::npos) << message;
    }
    EXPECT_NE(error("", json{{"transport", {{"host", "x"}}}}).find("transport.host"), std::string::npos);
    EXPECT_NE(error("", json{{"authoritative", {{"snapshotrate", 20}}}}).find("authoritative.snapshotrate"), std::string::npos);
    EXPECT_NE(error("", json{{"deterministic", 5}}).find("deterministic must be an object"), std::string::npos);

    // The message stays short for a long key
    const std::string longKey(500, 'k');
    EXPECT_LT(error("", json{{longKey, 1}}).size(), 400u);

    // Deterministic is refused with the card's reason, when the project's backend does not qualify
    const std::string refused = error("deterministic", json::object());
    EXPECT_NE(refused.find("The Deterministic preset can't be chosen"), std::string::npos) << refused;
    EXPECT_NE(refused.find("Requires a deterministic physics backend"), std::string::npos) << refused;

    // ... but not in a build and project that qualify, and not for another preset
    EXPECT_TRUE(Panel::BuildChangePatch(json{{"physics", {{"backend", "jolt"}, {"deterministic", true}}}}, "DETERMINISTIC",
                                        json::object(), FullBuild()));
    EXPECT_TRUE(Panel::BuildChangePatch(json::object(), "authoritative", json::object(), host));
    EXPECT_TRUE(Panel::BuildChangePatch(json::object(), "none", json::object(), host));
}

// ==================== Through the host ====================

TEST_F(EditorNetworkPanelTest, GetNetworkSettingsAnswersForAFreshProject)
{
    const json info = Get();
    EXPECT_EQ(info.at("preset"), "none");
    EXPECT_EQ(info.at("presets").size(), 3u);
    EXPECT_FALSE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("networkObjects"), 0);
    // The deterministic card follows the build the tests were compiled as
    // The project runs on the default backend (PhysX, else Jolt, else none) without physics.deterministic: none of those
    // declares CrossPlatform then, so the card is disabled whatever the build contains
    EXPECT_FALSE(PresetEntry(info, "deterministic").at("selectable").get<bool>());
    EXPECT_TRUE(PresetEntry(info, "deterministic").contains("unavailableReason"));
}

TEST_F(EditorNetworkPanelTest, SetNetworkSettingsSavesThePresetAndTunableSettingsAndKeepsTheRest)
{
    ASSERT_EQ(Execute(server, CommandType::SetProjectSettings,
                      JsonPayload({{"physics", {{"fixedTimestep", 0.01}}}, {"future", {{"keep", true}}}}))
                  .type,
              static_cast<uint8_t>(ResponseType::ProjectInfo));
    const std::size_t events = ProjectChangedEvents(server);

    const json info = JsonOf(Set("Authoritative", json{{"tickRate", 30}, {"authoritative", {{"snapshotRate", 20}}}}), SettingsType);
    EXPECT_EQ(info.at("preset"), "authoritative");
    EXPECT_EQ(info.at("tickRate"), 30);
    EXPECT_EQ(info.at("values").at("authoritative").at("snapshotRate"), 20);
    EXPECT_TRUE(PresetEntry(info, "authoritative").at("selected"));
    EXPECT_EQ(RowOf(info, "authoritative", "network.authoritative.snapshotRate").at("source"), "project");

    const json saved = SavedSettings();
    EXPECT_EQ(saved.at("network").at("preset"), "authoritative") << "saved in lower case";
    EXPECT_EQ(saved.at("network").at("tickRate"), 30);
    EXPECT_EQ(saved.at("network").at("authoritative").at("snapshotRate"), 20);
    EXPECT_EQ(saved.at("physics").at("fixedTimestep").get<double>(), 0.01);
    EXPECT_EQ(saved.at("future").at("keep"), true);
    EXPECT_GT(ProjectChangedEvents(server), events);

    // A later change merges, and a null returns a setting to its default
    const json again = JsonOf(Set("", json{{"authoritative", {{"smoothingMs", 50}, {"snapshotRate", nullptr}}}}), SettingsType);
    EXPECT_EQ(again.at("preset"), "authoritative") << "an empty preset keeps it";
    EXPECT_EQ(again.at("values").at("authoritative").at("snapshotRate"), 30);
    EXPECT_EQ(again.at("values").at("authoritative").at("smoothingMs"), 50);
    EXPECT_EQ(again.at("tickRate"), 30);
    EXPECT_FALSE(SavedSettings().at("network").at("authoritative").contains("snapshotRate"));

    // The same data through GetNetworkSettings
    EXPECT_EQ(Get(), again);
}

TEST_F(EditorNetworkPanelTest, SetNetworkSettingsRefusesBadRequestsAndSavesNothing)
{
    const std::string before = ProjectText();
    const std::size_t events = ProjectChangedEvents(server);

    const std::vector<std::pair<std::string, json>> requests = {
        {"lockstep", json::object()},
        {"", json::array()},
        {"", json{{"preset", "deterministic"}}},
        {"", json{{"overrides", {{"physics.gpu", "on"}}}}},
        {"", json{{"physics", {{"gpu", "on"}}}}},
        {"", json{{"tickRate", "fast"}}},
        {"", json{{"tickRate", 60.5}}},
        {"", json{{"tickRate", 600}}},
        {"", json{{"transport", {{"port", 0}}}}},
        {"", json{{"authoritative", {{"snapThreshold", "x"}}}}},
        {"", json{{"deterministic", {{"mode", "sync"}}}}},
        {"", json{{"transport", {{"udp", 1}}}}},
    };
    for (const auto &[preset, settings] : requests)
    {
        const Frame refused = Set(preset, settings);
        EXPECT_EQ(refused.type, ErrorType) << preset << " " << settings.dump();
        EXPECT_FALSE(refused.Text().empty());
        EXPECT_EQ(ProjectText(), before) << settings.dump();
    }
    EXPECT_EQ(ProjectChangedEvents(server), events);
    EXPECT_EQ(Get().at("preset"), "none");
}

TEST_F(EditorNetworkPanelTest, TheDeterministicPresetIsRefusedUntilTheBackendQualifiesAndNothingIsSaved)
{
    if (AnyBackendSelectable())
        GTEST_SKIP() << "this build has a backend that qualifies; the pure tests cover the refusal";
    const std::string before = ProjectText();
    const Frame refused = Set("deterministic", json::object());
    ASSERT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("Requires a deterministic physics backend"), std::string::npos) << refused.Text();
    EXPECT_EQ(ProjectText(), before);
    EXPECT_EQ(Get().at("preset"), "none");
}

TEST_F(EditorNetworkPanelTest, ARequestThatChangesNothingSavesNothing)
{
    const std::string before = ProjectText();
    const std::size_t events = ProjectChangedEvents(server);
    const json info = JsonOf(Set("", json::object()), SettingsType);
    EXPECT_EQ(info.at("preset"), "none");
    EXPECT_EQ(ProjectText(), before);
    EXPECT_EQ(ProjectChangedEvents(server), events);
}

TEST_F(EditorNetworkPanelTest, TheNudgeCountsTheOpenScenesNetworkObjectsAndStopsOnceAPresetIsChosen)
{
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Net Scene"})).type,
              static_cast<uint8_t>(ResponseType::SceneInfo));
    EXPECT_FALSE(Get().at("nudge").at("show")) << "an empty scene";

    Scene &scene = SceneManager::GetCurSceneRef();
    for (const char *name : {"A", "B"})
    {
        const auto object = GameObject::Create(name);
        object->AddComponent<Network::NetworkObject>();
        scene.AddRootGameObject(object);
    }
    scene.AddRootGameObject(GameObject::Create("Plain"));

    json info = Get();
    EXPECT_TRUE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("networkObjects"), 2);
    EXPECT_EQ(info.at("nudge").at("reasons"), json::array({"scene"}));

    info = JsonOf(Set("authoritative", json::object()), SettingsType);
    EXPECT_FALSE(info.at("nudge").at("show"));
    EXPECT_EQ(info.at("nudge").at("networkObjects"), 2);
}

TEST_F(EditorNetworkPanelTest, GetNetworkValidationReportsTheProjectsOwnPresetOrTheOneAskedFor)
{
    json report = Validate();
    EXPECT_EQ(report.at("preset"), "none");
    EXPECT_EQ(report.at("requirements").size(), 0u);

    // Asking for Deterministic does not change the project
    const std::string before = ProjectText();
    report = Validate("DETERMINISTIC");
    EXPECT_EQ(report.at("preset"), "deterministic");
    EXPECT_EQ(report.at("projectPreset"), "none");
    EXPECT_EQ(report.at("requirements").size(), 10u);
    EXPECT_TRUE(report.at("refused")) << "a fresh project has no Lua seed and no scalar SIMD tier";
    EXPECT_EQ(ProjectText(), before);
    // The facts of this build are what the engine says
    EXPECT_EQ(Requirement(report, "strict-fp").at("met").get<bool>(), Panel::CurrentHostBuild().strictFp);
    EXPECT_EQ(Requirement(report, "engine-trig").at("met").get<bool>(), Panel::CurrentHostBuild().contractionFree);
    EXPECT_FALSE(Requirement(report, "lua-seed").at("met").get<bool>());

    EXPECT_EQ(Validate("authoritative").at("preset"), "authoritative");

    // The project's own preset when none is named
    ASSERT_EQ(Set("authoritative", json::object()).type, SettingsType);
    EXPECT_EQ(Validate().at("preset"), "authoritative");
    EXPECT_EQ(Validate().at("projectPreset"), "authoritative");
}

TEST_F(EditorNetworkPanelTest, GetNetworkValidationRefusesAnUnknownPreset)
{
    for (const std::string name : {"lockstep", " none", "rollback"})
    {
        const Frame refused = Execute(server, CommandType::GetNetworkValidation, StringPayload(name));
        EXPECT_EQ(refused.type, ErrorType) << name;
        EXPECT_NE(refused.Text().find("Unknown network preset"), std::string::npos) << refused.Text();
    }
}

TEST_F(EditorNetworkPanelTest, FixesFromTheReportApplyThroughSetProjectSettings)
{
    json report = Validate("deterministic");
    json patch = json::object();
    for (const json &requirement : report.at("requirements"))
    {
        if (requirement.contains("fix") && requirement.at("id") != "physics-backend" && requirement.at("id") != "physics-threads")
            patch.merge_patch(requirement.at("fix").at("settings"));
    }
    ASSERT_TRUE(patch.contains("math")) << "scalar SIMD is a mechanical fix";
    ASSERT_EQ(Execute(server, CommandType::SetProjectSettings, JsonPayload(patch)).type,
              static_cast<uint8_t>(ResponseType::ProjectInfo));
    report = Validate("deterministic");
    EXPECT_TRUE(Requirement(report, "simd-scalar").at("met").get<bool>());
    EXPECT_TRUE(Requirement(report, "gpu-physics-off").at("met").get<bool>());
    EXPECT_TRUE(Requirement(report, "fixed-timestep").at("met").get<bool>());
}

TEST(EditorNetworkPanelNoProjectTest, ReadsNeedNoProjectAndTheSetterDoes)
{
    EditorServer server;
    ASSERT_FALSE(server.HasProject());
    const json info = JsonOf(Execute(server, CommandType::GetNetworkSettings), SettingsType);
    EXPECT_EQ(info.at("preset"), "none");
    EXPECT_EQ(info.at("presets").size(), 3u);
    EXPECT_FALSE(info.at("nudge").at("show"));

    const json report = JsonOf(Execute(server, CommandType::GetNetworkValidation, StringPayload("deterministic")), ValidationType);
    EXPECT_EQ(report.at("preset"), "deterministic");
    EXPECT_EQ(JsonOf(Execute(server, CommandType::GetNetworkValidation, StringPayload("")), ValidationType).at("preset"), "none");

    const Frame refused = Execute(server, CommandType::SetNetworkSettings, SetPayload("authoritative", json::object()));
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("No project"), std::string::npos) << refused.Text();
}

// ==================== Who may send them ====================

TEST_F(EditorNetworkPanelTest, ViewersAreRefusedTheSetterAndMayReadTheOthers)
{
    const auto id = [](CommandType type) { return static_cast<uint8_t>(type); };
    for (const CommandType read : {CommandType::GetNetworkSettings, CommandType::GetNetworkValidation})
    {
        EXPECT_EQ(EditorServer::ClassifyCommand(id(read)), EditorServer::CommandClass::Read);
        for (const TokenRole role : {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin})
            EXPECT_TRUE(EditorServer::RoleAllows(role, id(read))) << TokenRoleName(role);
    }
    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::SetNetworkSettings)), EditorServer::CommandClass::Write);
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id(CommandType::SetNetworkSettings)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id(CommandType::SetNetworkSettings)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id(CommandType::SetNetworkSettings)));
    EXPECT_NE(EditorServer::RoleDeniedMessage(TokenRole::Viewer, id(CommandType::SetNetworkSettings)).find("forbidden"),
              std::string::npos);
}

TEST_F(EditorNetworkPanelTest, AReadOnlyHostAnswersTheReadsAndRefusesTheSetter)
{
    ASSERT_TRUE(server.SetReadOnly(true));
    const std::string before = ProjectText();
    EXPECT_EQ(Execute(server, CommandType::GetNetworkSettings).type, SettingsType);
    EXPECT_EQ(Execute(server, CommandType::GetNetworkValidation, StringPayload("")).type, ValidationType);
    EXPECT_EQ(Set("authoritative", json::object()).type, ErrorType);
    EXPECT_EQ(ProjectText(), before);
}

TEST_F(EditorNetworkPanelTest, TheSetterWritesTheProjectSoAPlayHostRefusesItAndTheReadsAreAllowed)
{
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::SetNetworkSettings)));
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::GetNetworkSettings)));
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::GetNetworkValidation)));
}

TEST_F(EditorNetworkPanelTest, HelloAdvertisesTheNetworkPanel)
{
    const auto capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "networkPanel"), capabilities.end());
}
