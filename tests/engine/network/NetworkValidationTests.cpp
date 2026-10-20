#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <math/MathRegistrar.hpp>

#include "engine/Health.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/NetworkValidation.hpp"
#include "engine/physics/DeterminismLevel.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "../particles/FakePhysicsBackend.hpp"

// The startup checks of the network presets (ValidateNetwork), the Network health row built from them, and the
// determinism / snapshot / set-state declarations of the physics backends. Application::Init applies the same
// functions (its refusal is tested in HealthTests.cpp).

using namespace N2Engine;
using namespace N2Engine::Network;
using Physics::DeterminismLevel;
using Physics::GpuMode;

namespace
{
    NetworkSettings Preset(const NetworkPreset preset, const DeterministicMode mode = DeterministicMode::Lockstep)
    {
        NetworkSettings settings;
        settings.preset = preset;
        settings.deterministic.mode = mode;
        return settings;
    }

    /// An environment that meets every Deterministic requirement: what a project would have once a cross-platform
    /// backend, the engine trig and the tick loop exist. Nothing like it exists today.
    NetworkEnvironment EverythingMet()
    {
        NetworkEnvironment env;
        env.hasBackend = true;
        env.backendLevel = DeterminismLevel::CrossPlatform;
        env.backendSupportsSnapshot = true;
        env.backendSupportsSetState = true;
        env.strictFp = true;
        env.simdTier = Math::SIMDTierRequest::Scalar;
        env.luaSeedSet = true;
        env.engineOwnedTrig = true;
        env.ownedTickLoop = true;
        env.fixedTimestep = true;
        env.gpuMode = GpuMode::Off;
        return env;
    }

    std::vector<std::string> UnmetIds(const NetworkValidation &validation)
    {
        std::vector<std::string> ids;
        for (const NetworkRequirement *requirement : validation.Unmet())
        {
            ids.push_back(requirement->id);
        }
        return ids;
    }

    /// The environment of a real build today: what Application::Init sees for a default project on PhysX
    NetworkEnvironment TodayWithPhysX()
    {
        NetworkEnvironment env;
        env.hasBackend = true;
        env.backendLevel = DeterminismLevel::None;
        env.strictFp = false;
        env.simdTier = Math::SIMDTierRequest::Auto;
        return env;
    }
}

// ===== none =====

TEST(NetworkValidationTest, TheNonePresetChecksNothingWhateverTheEnvironment)
{
    for (const NetworkEnvironment &env : {NetworkEnvironment{}, TodayWithPhysX(), EverythingMet()})
    {
        const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::None), env);
        EXPECT_EQ(validation.preset, NetworkPreset::None);
        EXPECT_TRUE(validation.requirements.empty());
        EXPECT_TRUE(validation.warnings.empty());
        EXPECT_FALSE(validation.Refused());
        EXPECT_TRUE(validation.Unmet().empty());
    }
    NetworkEnvironment gpu = TodayWithPhysX();
    gpu.gpuMode = GpuMode::On;
    EXPECT_TRUE(ValidateNetwork(Preset(NetworkPreset::None), gpu).warnings.empty());
}

TEST(NetworkValidationTest, TheNoneRowIsDisabledAndSaysSoWhateverTheEnvironment)
{
    const NetworkSettings settings = Preset(NetworkPreset::None);
    const SubsystemStatus row = MakeNetworkStatus(settings, ValidateNetwork(settings, NetworkEnvironment{}));
    EXPECT_EQ(row.name, "Network");
    EXPECT_EQ(row.state, SubsystemState::Disabled);
    EXPECT_EQ(row.detail, "Preset none (offline): no network model");
    // The row is the same for a project without the block (default settings are the none preset)
    const SubsystemStatus fresh = MakeNetworkStatus(NetworkSettings{}, ValidateNetwork(NetworkSettings{}, EverythingMet()));
    EXPECT_EQ(fresh.state, row.state);
    EXPECT_EQ(fresh.detail, row.detail);
}

// ===== authoritative: warnings only =====

TEST(NetworkValidationTest, AuthoritativeWithAGoodEnvironmentHasNoWarnings)
{
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Authoritative), EverythingMet());
    EXPECT_TRUE(validation.warnings.empty());
    EXPECT_TRUE(validation.requirements.empty());
    EXPECT_FALSE(validation.Refused());
}

TEST(NetworkValidationTest, AuthoritativeWarnsAboutGpuPhysics)
{
    NetworkEnvironment env = EverythingMet();
    env.gpuMode = GpuMode::Auto;
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Authoritative), env);
    ASSERT_EQ(validation.warnings.size(), 1u);
    EXPECT_NE(validation.warnings[0].find("physics.gpu is auto"), std::string::npos);
    EXPECT_FALSE(validation.Refused());
}

TEST(NetworkValidationTest, AuthoritativeWarnsWhenTheBackendCannotSetState)
{
    NetworkEnvironment env = EverythingMet();
    env.backendSupportsSetState = false;
    NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Authoritative), env);
    ASSERT_EQ(validation.warnings.size(), 1u);
    EXPECT_NE(validation.warnings[0].find("SupportsSetState"), std::string::npos);

    env.hasBackend = false;
    validation = ValidateNetwork(Preset(NetworkPreset::Authoritative), env);
    ASSERT_EQ(validation.warnings.size(), 1u);
    EXPECT_NE(validation.warnings[0].find("no physics backend"), std::string::npos);
}

TEST(NetworkValidationTest, AuthoritativeWarnsNeverRefuses)
{
    // The worst case: GPU on, PhysX today (no set-state)
    NetworkEnvironment env = TodayWithPhysX();
    env.gpuMode = GpuMode::On;
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Authoritative), env);
    EXPECT_EQ(validation.warnings.size(), 2u);
    EXPECT_FALSE(validation.Refused());

    const SubsystemStatus row = MakeNetworkStatus(Preset(NetworkPreset::Authoritative), validation);
    EXPECT_EQ(row.state, SubsystemState::Disabled);
    EXPECT_NE(row.detail.find("Preset authoritative"), std::string::npos);
    EXPECT_NE(row.detail.find("warning: physics.gpu is on"), std::string::npos);
    EXPECT_NE(row.detail.find("SupportsSetState"), std::string::npos);
}

// ===== deterministic: refusal =====

TEST(NetworkValidationTest, DeterministicTodayNamesEveryUnmetRequirement)
{
    // A default project on PhysX: the Deterministic preset cannot start. Each reason is listed, in this order.
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Deterministic), TodayWithPhysX());
    EXPECT_TRUE(validation.Refused());
    EXPECT_EQ(UnmetIds(validation),
              (std::vector<std::string>{"physics-backend", "strict-fp", "simd-scalar", "lua-seed", "engine-trig",
                                        "physics-threads", "tick-loop"}));
    // gpu-physics-off and fixed-timestep are met and listed as met; lockstep has no snapshot requirement
    EXPECT_EQ(validation.requirements.size(), 9u);
    for (const NetworkRequirement &requirement : validation.requirements)
    {
        EXPECT_EQ(requirement.detail.empty(), requirement.met) << requirement.id;
        EXPECT_FALSE(requirement.text.empty()) << requirement.id;
    }
}

TEST(NetworkValidationTest, DeterministicWithAPhysicsDeterministicPhysXStillCannotStart)
{
    // physics.deterministic gives PhysX SameBinary and 0 threads: the threads requirement is met, the backend is not
    NetworkEnvironment env = TodayWithPhysX();
    env.backendLevel = DeterminismLevel::SameBinary;
    env.strictFp = true;
    env.simdTier = Math::SIMDTierRequest::Scalar;
    env.luaSeedSet = true;
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Deterministic), env);
    EXPECT_TRUE(validation.Refused());
    EXPECT_EQ(UnmetIds(validation), (std::vector<std::string>{"physics-backend", "engine-trig", "tick-loop"}));
}

TEST(NetworkValidationTest, DeterministicWithoutABackendNamesIt)
{
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Deterministic), NetworkEnvironment{});
    EXPECT_TRUE(validation.Refused());
    const auto &backend = validation.requirements.front();
    EXPECT_EQ(backend.id, "physics-backend");
    EXPECT_FALSE(backend.met);
    EXPECT_NE(backend.detail.find("no physics backend"), std::string::npos);
}

TEST(NetworkValidationTest, DeterministicWithEverythingMetIsNotRefused)
{
    // Hypothetical (no such backend, trig or loop exists yet): the checks themselves pass when they are all there
    const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Deterministic), EverythingMet());
    EXPECT_FALSE(validation.Refused());
    EXPECT_TRUE(validation.Unmet().empty());
    EXPECT_EQ(validation.requirements.size(), 9u);
    const SubsystemStatus row = MakeNetworkStatus(Preset(NetworkPreset::Deterministic), validation);
    EXPECT_EQ(row.state, SubsystemState::Disabled);
}

TEST(NetworkValidationTest, EachDeterministicRequirementCanRefuseOnItsOwn)
{
    struct Case
    {
        const char *id;
        void (*spoil)(NetworkEnvironment &);
    };
    const Case cases[] = {
        {"physics-backend", [](NetworkEnvironment &e) { e.backendLevel = DeterminismLevel::SameBinary; }},
        {"physics-backend", [](NetworkEnvironment &e) { e.hasBackend = false; }},
        {"strict-fp", [](NetworkEnvironment &e) { e.strictFp = false; }},
        {"simd-scalar", [](NetworkEnvironment &e) { e.simdTier = Math::SIMDTierRequest::Auto; }},
        {"simd-scalar", [](NetworkEnvironment &e) { e.simdTier = Math::SIMDTierRequest::AVX; }},
        {"lua-seed", [](NetworkEnvironment &e) { e.luaSeedSet = false; }},
        {"engine-trig", [](NetworkEnvironment &e) { e.engineOwnedTrig = false; }},
        {"gpu-physics-off", [](NetworkEnvironment &e) { e.gpuMode = GpuMode::Auto; }},
        {"gpu-physics-off", [](NetworkEnvironment &e) { e.gpuMode = GpuMode::On; }},
        {"fixed-timestep", [](NetworkEnvironment &e) { e.fixedTimestep = false; }},
        {"tick-loop", [](NetworkEnvironment &e) { e.ownedTickLoop = false; }},
        {"fixed-timestep", [](NetworkEnvironment &e) { e.fixedTimestepSeconds = 0.02; }},
    };
    for (const Case &c : cases)
    {
        NetworkEnvironment env = EverythingMet();
        c.spoil(env);
        const NetworkValidation validation = ValidateNetwork(Preset(NetworkPreset::Deterministic), env);
        EXPECT_TRUE(validation.Refused()) << c.id;
        const auto ids = UnmetIds(validation);
        EXPECT_NE(std::ranges::find(ids, std::string(c.id)), ids.end()) << c.id;
    }
    // A backend that declares None fails both the backend and the thread-independence requirement
    NetworkEnvironment none = EverythingMet();
    none.backendLevel = DeterminismLevel::None;
    EXPECT_EQ(UnmetIds(ValidateNetwork(Preset(NetworkPreset::Deterministic), none)),
              (std::vector<std::string>{"physics-backend", "physics-threads"}));
}

TEST(NetworkValidationTest, TheFixedTimestepMustMatchTheTickRate)
{
    NetworkSettings settings = Preset(NetworkPreset::Deterministic);
    settings.tickRate = 60;
    NetworkEnvironment env = EverythingMet();

    env.fixedTimestepSeconds = 1.0 / 60.0;
    EXPECT_FALSE(ValidateNetwork(settings, env).Refused());
    env.fixedTimestepSeconds = 0.0166667; // what a project file holds
    EXPECT_FALSE(ValidateNetwork(settings, env).Refused());
    env.fixedTimestepSeconds = 0.02;
    const NetworkValidation mismatch = ValidateNetwork(settings, env);
    EXPECT_EQ(UnmetIds(mismatch), (std::vector<std::string>{"fixed-timestep"}));
    EXPECT_NE(mismatch.Unmet()[0]->detail.find("network.tickRate 60"), std::string::npos);
    settings.tickRate = 50;
    EXPECT_FALSE(ValidateNetwork(settings, env).Refused());
}

TEST(NetworkValidationTest, RollbackAlsoNeedsASnapshotLockstepDoesNot)
{
    NetworkEnvironment env = EverythingMet();
    env.backendSupportsSnapshot = false;

    const NetworkValidation lockstep =
        ValidateNetwork(Preset(NetworkPreset::Deterministic, DeterministicMode::Lockstep), env);
    EXPECT_FALSE(lockstep.Refused());

    const NetworkValidation rollback =
        ValidateNetwork(Preset(NetworkPreset::Deterministic, DeterministicMode::Rollback), env);
    EXPECT_TRUE(rollback.Refused());
    EXPECT_EQ(UnmetIds(rollback), (std::vector<std::string>{"snapshot"}));
    EXPECT_EQ(rollback.requirements.size(), 10u);

    env.backendSupportsSnapshot = true;
    EXPECT_FALSE(ValidateNetwork(Preset(NetworkPreset::Deterministic, DeterministicMode::Rollback), env).Refused());
}

TEST(NetworkValidationTest, TheRefusedRowIsFailedAndNamesEveryUnmetRequirement)
{
    const NetworkSettings settings = Preset(NetworkPreset::Deterministic);
    const NetworkValidation validation = ValidateNetwork(settings, TodayWithPhysX());
    const SubsystemStatus row = MakeNetworkStatus(settings, validation);
    EXPECT_EQ(row.name, "Network");
    EXPECT_EQ(row.state, SubsystemState::Failed);
    EXPECT_NE(row.detail.find("Preset deterministic: refused to start, 7 unmet requirements"), std::string::npos)
        << row.detail;
    for (const NetworkRequirement *requirement : validation.Unmet())
    {
        EXPECT_NE(row.detail.find(requirement->text), std::string::npos) << requirement->id;
        EXPECT_NE(row.detail.find(requirement->detail), std::string::npos) << requirement->id;
    }
    // A met requirement is not in the list
    EXPECT_EQ(row.detail.find("physics.gpu is off"), std::string::npos);
    EXPECT_NE(row.detail.find("engine-owned trig (C1)"), std::string::npos);

    // One unmet requirement is singular
    NetworkEnvironment one = EverythingMet();
    one.luaSeedSet = false;
    const SubsystemStatus single = MakeNetworkStatus(settings, ValidateNetwork(settings, one));
    EXPECT_EQ(single.state, SubsystemState::Failed);
    EXPECT_NE(single.detail.find("1 unmet requirement:"), std::string::npos) << single.detail;
}

TEST(NetworkValidationTest, TheRowIsAGenericSubsystemStatusSoNoProtocolChangeIsNeeded)
{
    // The engine health is a list of {name, state, detail}: the Network row is one more entry of it
    EngineHealth health;
    health.subsystems.push_back(MakeNetworkStatus(NetworkSettings{}, ValidateNetwork(NetworkSettings{}, NetworkEnvironment{})));
    EXPECT_TRUE(health.IsHealthy());
    ASSERT_NE(health.Find("Network"), nullptr);
    EXPECT_EQ(ToString(health.Find("Network")->state), "Disabled");

    const NetworkSettings det = Preset(NetworkPreset::Deterministic);
    health.subsystems.push_back(MakeNetworkStatus(det, ValidateNetwork(det, NetworkEnvironment{})));
    EXPECT_FALSE(health.IsHealthy());
}

// ===== the backends' declarations =====

TEST(PhysicsBackendDeclarationTest, ABackendDeclaresNothingByDefault)
{
    const TestSupport::FakePhysicsBackend fake(false);
    EXPECT_EQ(fake.GetDeterminism(), DeterminismLevel::None);
    EXPECT_FALSE(fake.SupportsSnapshot());
    EXPECT_FALSE(fake.SupportsSetState());
}

TEST(PhysicsBackendDeclarationTest, DeterminismLevelsAreOrderedAndNamed)
{
    EXPECT_LT(DeterminismLevel::None, DeterminismLevel::SameBinary);
    EXPECT_LT(DeterminismLevel::SameBinary, DeterminismLevel::CrossPlatform);
    EXPECT_EQ(Physics::ToString(DeterminismLevel::None), "None");
    EXPECT_EQ(Physics::ToString(DeterminismLevel::SameBinary), "SameBinary");
    EXPECT_EQ(Physics::ToString(DeterminismLevel::CrossPlatform), "CrossPlatform");
}

TEST(PhysicsBackendDeclarationTest, PhysXDeclaresSameBinaryOnlyWithTheDeterministicOptionAndNoWorkerThreads)
{
    using Physics::PhysXBackend;
    // The static form Application::Init uses before any backend exists
    EXPECT_EQ(PhysXBackend::DeclaredDeterminism(false), DeterminismLevel::None);
    EXPECT_EQ(PhysXBackend::DeclaredDeterminism(true), DeterminismLevel::SameBinary);
    EXPECT_EQ(PhysXBackend::DeclaredDeterminism(true, 0u), DeterminismLevel::SameBinary);
    EXPECT_EQ(PhysXBackend::DeclaredDeterminism(true, 4u), DeterminismLevel::None);
    EXPECT_EQ(PhysXBackend::DeclaredDeterminism(false, 0u), DeterminismLevel::None);
    // Never CrossPlatform: Windows and Linux use different PhysX binaries
    for (const bool deterministic : {false, true})
    {
        EXPECT_NE(PhysXBackend::DeclaredDeterminism(deterministic), DeterminismLevel::CrossPlatform);
    }
    EXPECT_FALSE(PhysXBackend::kSupportsSnapshot);
    EXPECT_FALSE(PhysXBackend::kSupportsSetState);
}

#ifdef N2ENGINE_PHYSX_ENABLED

TEST(PhysicsBackendDeclarationTest, ANotYetInitializedPhysXBackendDeclaresFromItsConfiguration)
{
    // No Initialize: the declarations read the configuration only, so no PhysX scene is created
    Physics::PhysXBackend backend;
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::None);
    EXPECT_FALSE(backend.SupportsSnapshot());
    // SupportsSetState stays false until the body-state API (teleporting a dynamic body, sleep state) exists
    EXPECT_FALSE(backend.SupportsSetState());

    backend.SetDeterministic(true);
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::SameBinary);
    EXPECT_FALSE(backend.SupportsSnapshot());
    EXPECT_FALSE(backend.SupportsSetState());

    backend.SetWorkerThreadsForTesting(4u);
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::None);
    backend.SetWorkerThreadsForTesting(0u);
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::SameBinary);

    // GPU physics is refused together with the deterministic option (the CPU scene is used), so the level holds
    backend.SetGpuMode(GpuMode::On);
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::SameBinary);
    backend.SetDeterministic(false);
    EXPECT_EQ(backend.GetDeterminism(), DeterminismLevel::None);
}

#endif // N2ENGINE_PHYSX_ENABLED
