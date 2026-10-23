#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/ProjectSettings.hpp"
#include "engine/physics/PhysicsBackendKind.hpp"

// The physics.backend project setting (ReadPhysicsBackend) and the selection rule Application::Init applies to it
// (ChooseBackend). Pure functions: no backend is created. Application::Init itself opens a window and a physics scene,
// so a start that goes ahead is not run in this process (see docs/testing.html); the Deterministic refusal for an
// unavailable backend is tested through ValidateNetwork in tests/engine/network/NetworkValidationTests.cpp.
//
// No `using namespace N2Engine`: math/MathRegistrar.hpp (included by ProjectSettings.hpp) forward-declares
// N2Engine::Vector3 and N2Engine::Quaternion, which would make the Math types ambiguous.

namespace
{
    using json = nlohmann::json;
    using N2Engine::Physics::BackendAvailability;
    using N2Engine::Physics::BackendChoice;
    using N2Engine::Physics::BackendRequestSource;
    using N2Engine::Physics::ChooseBackend;
    using N2Engine::Physics::PhysicsBackendKind;

    std::optional<PhysicsBackendKind> Read(const json &settings, std::vector<std::string> *problems = nullptr)
    {
        return N2Engine::ReadPhysicsBackend(settings, problems);
    }
}

// ===== the names =====

TEST(PhysicsBackendKindTest, TheNamesParseCaseInsensitivelyAndNothingElseDoes)
{
    using N2Engine::Physics::ParsePhysicsBackendKind;
    EXPECT_EQ(ParsePhysicsBackendKind("physx"), PhysicsBackendKind::PhysX);
    EXPECT_EQ(ParsePhysicsBackendKind("PhysX"), PhysicsBackendKind::PhysX);
    EXPECT_EQ(ParsePhysicsBackendKind("jolt"), PhysicsBackendKind::Jolt);
    EXPECT_EQ(ParsePhysicsBackendKind("JOLT"), PhysicsBackendKind::Jolt);
    for (const char *bad : {"", " jolt", "jolt ", "box2d", "physx5", "auto"})
    {
        EXPECT_FALSE(ParsePhysicsBackendKind(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(PhysicsBackendKindTest, TheNamesRoundTripAndTheDisplayNamesAreForPeople)
{
    using N2Engine::Physics::DisplayName;
    using N2Engine::Physics::ParsePhysicsBackendKind;
    using N2Engine::Physics::ToString;
    for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
    {
        EXPECT_EQ(ParsePhysicsBackendKind(ToString(kind)), kind);
    }
    EXPECT_EQ(ToString(PhysicsBackendKind::PhysX), "physx");
    EXPECT_EQ(ToString(PhysicsBackendKind::Jolt), "jolt");
    EXPECT_EQ(DisplayName(PhysicsBackendKind::PhysX), "PhysX");
    EXPECT_EQ(DisplayName(PhysicsBackendKind::Jolt), "Jolt");
}

TEST(PhysicsBackendKindTest, EachBackendIsInTheBuildOnlyWithItsOption)
{
    using N2Engine::Physics::IsBackendCompiledIn;
#ifdef N2ENGINE_PHYSX_ENABLED
    EXPECT_TRUE(IsBackendCompiledIn(PhysicsBackendKind::PhysX));
#else
    EXPECT_FALSE(IsBackendCompiledIn(PhysicsBackendKind::PhysX));
#endif
    // N2ENGINE_USE_JOLT puts the Jolt backend into the engine (N2ENGINE_JOLT_ENABLED)
#ifdef N2ENGINE_JOLT_ENABLED
    EXPECT_TRUE(IsBackendCompiledIn(PhysicsBackendKind::Jolt));
#else
    EXPECT_FALSE(IsBackendCompiledIn(PhysicsBackendKind::Jolt));
#endif
}

TEST(PhysicsBackendKindTest, CompiledInBackendsIsWhatEachOptionPutsIntoTheBuild)
{
    using N2Engine::Physics::IsBackendCompiledIn;
    const N2Engine::Physics::BackendAvailability available = N2Engine::Physics::CompiledInBackends();
    EXPECT_EQ(available.physx, IsBackendCompiledIn(PhysicsBackendKind::PhysX));
    EXPECT_EQ(available.jolt, IsBackendCompiledIn(PhysicsBackendKind::Jolt));
    // In this build, a selection only ever names a backend that is in it
    for (const bool deterministic : {false, true})
    {
        for (const auto source : {BackendRequestSource::HostOption, BackendRequestSource::ProjectSetting})
        {
            for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
            {
                const BackendChoice choice = ChooseBackend(kind, source, deterministic, available);
                if (choice.selected)
                {
                    EXPECT_TRUE(IsBackendCompiledIn(*choice.selected));
                }
            }
        }
        const BackendChoice automatic = ChooseBackend(std::nullopt, BackendRequestSource::Auto, deterministic, available);
        EXPECT_EQ(automatic.selected.has_value(), available.Any());
    }
}

// ===== the setting =====

TEST(PhysicsBackendSettingTest, IsUnsetWhenAbsentOrNull)
{
    std::vector<std::string> problems;
    EXPECT_FALSE(Read(json::object(), &problems).has_value());
    EXPECT_FALSE(Read(json::array(), &problems).has_value());
    EXPECT_FALSE(Read(json{{"physics", json::object()}}, &problems).has_value());
    EXPECT_FALSE(Read(json{{"physics", 5}}, &problems).has_value());
    EXPECT_FALSE(Read(json{{"physics", {{"backend", nullptr}}}}, &problems).has_value());
    EXPECT_TRUE(problems.empty());
}

TEST(PhysicsBackendSettingTest, ReadsPhysxAndJoltInAnyCase)
{
    std::vector<std::string> problems;
    EXPECT_EQ(Read(json{{"physics", {{"backend", "physx"}}}}, &problems), PhysicsBackendKind::PhysX);
    EXPECT_EQ(Read(json{{"physics", {{"backend", "jolt"}}}}, &problems), PhysicsBackendKind::Jolt);
    EXPECT_EQ(Read(json{{"physics", {{"backend", "Jolt"}}}}, &problems), PhysicsBackendKind::Jolt);
    EXPECT_EQ(Read(json{{"physics", {{"backend", "PHYSX"}}}}, &problems), PhysicsBackendKind::PhysX);
    EXPECT_TRUE(problems.empty());
}

TEST(PhysicsBackendSettingTest, ABadValueMeansPhysxWithOneWarning)
{
    for (const json &bad : {json(5), json(true), json("box2d"), json(""), json::array(), json::object()})
    {
        std::vector<std::string> problems;
        EXPECT_EQ(Read(json{{"physics", {{"backend", bad}}}}, &problems), PhysicsBackendKind::PhysX) << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems.front().find("physics.backend"), std::string::npos) << problems.front();
        EXPECT_NE(problems.front().find("using physx"), std::string::npos) << problems.front();
        // Without a problems list it is the same answer and no crash
        EXPECT_EQ(Read(json{{"physics", {{"backend", bad}}}}), PhysicsBackendKind::PhysX) << bad.dump();
    }
}

TEST(PhysicsBackendSettingTest, ApplyProjectSettingsValidatesItAndNeedsNoBackend)
{
    // A change needs a restart: applying only validates, so a good value reports nothing and a bad one says so once
    EXPECT_TRUE(
        N2Engine::ApplyProjectSettings(json{{"physics", {{"backend", "jolt"}}}}, std::set<std::string>{"physics"})
            .empty());
    const auto problems =
        N2Engine::ApplyProjectSettings(json{{"physics", {{"backend", "unreal"}}}}, std::set<std::string>{"physics"});
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems.front().find("physics.backend"), std::string::npos);
}

// ===== the selection rule =====
//
// ChooseBackend over every build ChooseBackend can see (PhysX and Jolt each in or out of the build), every request
// (nobody, the host option, the project setting; PhysX or Jolt) and both kinds of preset (Deterministic or not).

namespace
{
    constexpr BackendAvailability kBoth{.physx = true, .jolt = true};
    constexpr BackendAvailability kPhysXOnly{.physx = true, .jolt = false};
    constexpr BackendAvailability kJoltOnly{.physx = false, .jolt = true};
    constexpr BackendAvailability kNeither{.physx = false, .jolt = false};
    constexpr BackendAvailability kEveryBuild[] = {kBoth, kPhysXOnly, kJoltOnly, kNeither};
    constexpr BackendRequestSource kRequestSources[] = {BackendRequestSource::HostOption,
                                                        BackendRequestSource::ProjectSetting};

    std::string Contains(const std::string &message, const std::string &part)
    {
        return message.find(part) != std::string::npos ? std::string{} : "'" + message + "' lacks '" + part + "'";
    }
}

TEST(ChooseBackendTest, NobodyAskingSelectsPhysxThenJoltThenNone)
{
    for (const bool deterministic : {false, true})
    {
        const BackendChoice both = ChooseBackend(std::nullopt, BackendRequestSource::Auto, deterministic, kBoth);
        EXPECT_EQ(both.selected, PhysicsBackendKind::PhysX);
        EXPECT_TRUE(both.message.empty()) << both.message;

        EXPECT_EQ(ChooseBackend(std::nullopt, BackendRequestSource::Auto, deterministic, kPhysXOnly).selected,
                  PhysicsBackendKind::PhysX);

        // A Jolt-only build runs Jolt for a project that does not say (it used to get no physics at all)
        const BackendChoice jolt = ChooseBackend(std::nullopt, BackendRequestSource::Auto, deterministic, kJoltOnly);
        EXPECT_EQ(jolt.selected, PhysicsBackendKind::Jolt);
        EXPECT_FALSE(jolt.fellBack);
        EXPECT_FALSE(jolt.refused);
        EXPECT_TRUE(jolt.message.empty()) << jolt.message;

        const BackendChoice none = ChooseBackend(std::nullopt, BackendRequestSource::Auto, deterministic, kNeither);
        EXPECT_FALSE(none.selected.has_value());
        EXPECT_FALSE(none.fellBack);
        EXPECT_FALSE(none.refused);
        EXPECT_EQ(Contains(none.message, "this build has no physics backend"), "");
        for (const BackendChoice &choice : {both, jolt, none})
        {
            EXPECT_FALSE(choice.requested.has_value());
            EXPECT_EQ(choice.source, BackendRequestSource::Auto);
        }
    }
}

TEST(ChooseBackendTest, ABackendInTheBuildIsSelectedAsAskedWhateverTheOtherOne)
{
    for (const BackendAvailability available : kEveryBuild)
    {
        for (const BackendRequestSource source : kRequestSources)
        {
            for (const bool deterministic : {false, true})
            {
                for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
                {
                    if (!available.Has(kind))
                    {
                        continue;
                    }
                    const BackendChoice choice = ChooseBackend(kind, source, deterministic, available);
                    EXPECT_EQ(choice.requested, kind);
                    EXPECT_EQ(choice.source, source);
                    EXPECT_EQ(choice.selected, kind);
                    EXPECT_FALSE(choice.fellBack);
                    EXPECT_FALSE(choice.refused);
                    EXPECT_TRUE(choice.message.empty()) << choice.message;
                }
            }
        }
    }
}

TEST(ChooseBackendTest, AMissingBackendFallsBackOnlyToOneThatIsInTheBuild)
{
    // Not Deterministic: Jolt missing -> PhysX, PhysX missing -> Jolt, each with a warning that names it
    const BackendChoice toPhysx = ChooseBackend(PhysicsBackendKind::Jolt, BackendRequestSource::ProjectSetting, false,
                                                kPhysXOnly);
    EXPECT_EQ(toPhysx.selected, PhysicsBackendKind::PhysX);
    EXPECT_TRUE(toPhysx.fellBack);
    EXPECT_FALSE(toPhysx.refused);
    EXPECT_EQ(Contains(toPhysx.message, "physics.backend is jolt but this build has no Jolt backend"), "");
    EXPECT_EQ(Contains(toPhysx.message, "using physx instead"), "");

    const BackendChoice toJolt = ChooseBackend(PhysicsBackendKind::PhysX, BackendRequestSource::ProjectSetting, false,
                                               kJoltOnly);
    EXPECT_EQ(toJolt.selected, PhysicsBackendKind::Jolt);
    EXPECT_TRUE(toJolt.fellBack);
    EXPECT_FALSE(toJolt.refused);
    EXPECT_EQ(Contains(toJolt.message, "physics.backend is physx but this build has no PhysX backend"), "");
    EXPECT_EQ(Contains(toJolt.message, "using jolt instead"), "");
}

TEST(ChooseBackendTest, AMissingBackendWithNoOtherInTheBuildIsRefusedAndNamesNoBackend)
{
    for (const BackendRequestSource source : kRequestSources)
    {
        for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
        {
            const BackendChoice choice = ChooseBackend(kind, source, false, kNeither);
            EXPECT_FALSE(choice.selected.has_value());
            EXPECT_TRUE(choice.refused);
            EXPECT_FALSE(choice.fellBack);
            EXPECT_EQ(Contains(choice.message, "this build has no physics backend at all"), "");
            // Never "using ... instead": there is nothing to use
            EXPECT_EQ(choice.message.find("instead"), std::string::npos) << choice.message;
        }
    }
}

TEST(ChooseBackendTest, AMissingBackendRefusesADeterministicProjectAndNeverFallsBack)
{
    for (const BackendAvailability available : kEveryBuild)
    {
        for (const BackendRequestSource source : kRequestSources)
        {
            for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
            {
                if (available.Has(kind))
                {
                    continue;
                }
                const BackendChoice choice = ChooseBackend(kind, source, true, available);
                EXPECT_TRUE(choice.refused);
                EXPECT_FALSE(choice.fellBack);
                // Nothing else is put in its place, even when the other backend is in the build
                EXPECT_FALSE(choice.selected.has_value());
                EXPECT_EQ(choice.requested, kind);
                EXPECT_EQ(Contains(choice.message, "Deterministic"), "");
                EXPECT_EQ(Contains(choice.message, "never falls back"), "");
            }
        }
    }
}

TEST(ChooseBackendTest, EveryMessageNamesWhereTheRequestCameFrom)
{
    // The host option (no physics.backend) is not called a project setting, and the other way round
    const BackendChoice host = ChooseBackend(PhysicsBackendKind::Jolt, BackendRequestSource::HostOption, false,
                                             kPhysXOnly);
    EXPECT_EQ(Contains(host.message, "the host option (ApplicationOptions::physicsBackend) is jolt and the project "
                                     "sets no physics.backend"),
              "");
    EXPECT_EQ(host.message.find("physics.backend is"), std::string::npos) << host.message;

    const BackendChoice project = ChooseBackend(PhysicsBackendKind::Jolt, BackendRequestSource::ProjectSetting, false,
                                                kPhysXOnly);
    EXPECT_EQ(Contains(project.message, "physics.backend is jolt"), "");
    EXPECT_EQ(project.message.find("host option"), std::string::npos) << project.message;

    for (const bool deterministic : {false, true})
    {
        const BackendChoice refused =
            ChooseBackend(PhysicsBackendKind::PhysX, BackendRequestSource::HostOption, deterministic, kNeither);
        EXPECT_EQ(Contains(refused.message, "the host option (ApplicationOptions::physicsBackend) is physx"), "");
    }
    EXPECT_EQ(N2Engine::Physics::DescribeBackendRequest("jolt", BackendRequestSource::ProjectSetting),
              "physics.backend is jolt");
}
