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
    using N2Engine::Physics::BackendChoice;
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

TEST(PhysicsBackendKindTest, PhysXIsInTheBuildOnlyWithPhysXAndJoltIsNotInAnyBuildYet)
{
    using N2Engine::Physics::IsBackendCompiledIn;
#ifdef N2ENGINE_PHYSX_ENABLED
    EXPECT_TRUE(IsBackendCompiledIn(PhysicsBackendKind::PhysX));
#else
    EXPECT_FALSE(IsBackendCompiledIn(PhysicsBackendKind::PhysX));
#endif
    EXPECT_FALSE(IsBackendCompiledIn(PhysicsBackendKind::Jolt));
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

TEST(ChooseBackendTest, ABackendInTheBuildIsSelectedAsAskedForEveryPreset)
{
    for (const bool deterministic : {false, true})
    {
        for (const PhysicsBackendKind kind : {PhysicsBackendKind::PhysX, PhysicsBackendKind::Jolt})
        {
            const BackendChoice choice = ChooseBackend(kind, deterministic, true);
            EXPECT_EQ(choice.requested, kind);
            EXPECT_EQ(choice.selected, kind);
            EXPECT_FALSE(choice.fellBack);
            EXPECT_FALSE(choice.refused);
            EXPECT_TRUE(choice.message.empty()) << choice.message;
        }
    }
}

TEST(ChooseBackendTest, AnUnavailableJoltFallsBackToPhysxWithAWarningOutsideDeterministic)
{
    const BackendChoice choice = ChooseBackend(PhysicsBackendKind::Jolt, false, false);
    EXPECT_EQ(choice.requested, PhysicsBackendKind::Jolt);
    EXPECT_EQ(choice.selected, PhysicsBackendKind::PhysX);
    EXPECT_TRUE(choice.fellBack);
    EXPECT_FALSE(choice.refused);
    EXPECT_NE(choice.message.find("physics.backend is jolt"), std::string::npos) << choice.message;
    EXPECT_NE(choice.message.find("using physx instead"), std::string::npos) << choice.message;
}

TEST(ChooseBackendTest, AnUnavailableJoltRefusesADeterministicProjectAndNeverFallsBack)
{
    const BackendChoice choice = ChooseBackend(PhysicsBackendKind::Jolt, true, false);
    EXPECT_TRUE(choice.refused);
    EXPECT_FALSE(choice.fellBack);
    // Nothing else is put in its place
    EXPECT_EQ(choice.selected, PhysicsBackendKind::Jolt);
    EXPECT_NE(choice.message.find("Deterministic"), std::string::npos) << choice.message;
    EXPECT_NE(choice.message.find("never falls back"), std::string::npos) << choice.message;
}

TEST(ChooseBackendTest, APhysxRequestIsLeftToInitsBuiltWithoutPhysxPath)
{
    // A build without PhysX has nothing to fall back to: the choice stays PhysX (Init reports "Built without PhysX"
    // and the Deterministic check finds no backend), it never becomes a refusal of its own
    for (const bool deterministic : {false, true})
    {
        const BackendChoice choice = ChooseBackend(PhysicsBackendKind::PhysX, deterministic, false);
        EXPECT_EQ(choice.selected, PhysicsBackendKind::PhysX);
        EXPECT_FALSE(choice.fellBack);
        EXPECT_FALSE(choice.refused);
        EXPECT_TRUE(choice.message.empty());
    }
}
