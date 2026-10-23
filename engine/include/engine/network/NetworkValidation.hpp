#pragma once

#include <string>
#include <vector>

#include <math/MathRegistrar.hpp>

#include "engine/Health.hpp"
#include "engine/SimulationClock.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/physics/DeterminismLevel.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/PhysicsBackendKind.hpp"
#include "engine/physics/PhysicsFeatures.hpp"

namespace N2Engine::Network
{
    /// What the running build, the physics backend and the project settings offer, as the startup checks see it.
    /// Application::Init fills it in; tests fill it in by hand to reach every combination.
    struct NetworkEnvironment
    {
        /// A physics backend exists (it may still declare nothing)
        bool hasBackend = false;
        /// The backend physics.backend selected, as Physics::ToString names it ("physx"); empty when unknown (a test that
        /// fills in the environment by hand). Only used in the unmet-requirement text.
        std::string backendName;
        /// The project asked for a backend this build does not contain (physics.backend jolt before the Jolt backend
        /// exists) and no other was put in its place: hasBackend is false and the physics-backend requirement says so
        bool backendUnavailable = false;
        /// The backend named by backendUnavailable
        std::string backendRequested;
        /// Who asked for backendRequested: the project's physics.backend or the host's option (the message names it)
        Physics::BackendRequestSource backendRequestSource = Physics::BackendRequestSource::ProjectSetting;
        /// The feature groups the backend implements (its GetImplementedFeatures, or the class's kImplementedFeatures
        /// before it exists); the Deterministic preset needs all of them
        Physics::PhysicsFeatureSet backendFeatures;
        Physics::DeterminismLevel backendLevel = Physics::DeterminismLevel::None;
        bool backendSupportsSnapshot = false;
        bool backendSupportsSetState = false;
        /// Math::IsStrictFp(): this build has the strict floating-point profile
        bool strictFp = false;
        /// math.simdTier as the project requested it
        Math::SIMDTierRequest simdTier = Math::SIMDTierRequest::Auto;
        /// determinism.luaSeed is set
        bool luaSeedSet = false;
        /// The engine-owned Sin/Cos/Atan2/... (Math::Sin, math/DeterministicMath.hpp; netcode item C1) run without
        /// floating-point contraction in this build: Math::IsContractionFree(), which Application::Init sets
        bool engineOwnedTrig = false;
        /// The fixed-step loop runs as the netcode's tick loop (SimulationClock in a network SimulationMode: whole ticks
        /// owed, never dropped, and a fixed dt independent of timeScale). Application::Init sets it when network.preset
        /// selects a network mode (SimulationModeFor), which it then applies with Application::SetSimulationMode.
        bool ownedTickLoop = false;
        /// The fixed timestep is on (it always is: the engine has no way to switch it off)
        bool fixedTimestep = true;
        /// The physics fixed timestep in seconds (physics.fixedTimestep, else the engine default); 0: not known, which
        /// is not checked against network.tickRate. Deterministic needs it to equal 1 / tickRate.
        double fixedTimestepSeconds = 0.0;
        /// physics.gpu
        Physics::GpuMode gpuMode = Physics::GpuMode::Off;
    };

    /// One startup requirement of the Deterministic preset
    struct NetworkRequirement
    {
        std::string id;     // stable, e.g. "physics-backend"
        std::string text;   // what is required
        bool met = false;
        std::string detail; // why it is unmet; empty when met
    };

    /// The outcome of the startup checks for a project's preset
    struct NetworkValidation
    {
        NetworkPreset preset = NetworkPreset::None;
        /// Deterministic only: every requirement, met or not, in a fixed order. Empty for the other presets.
        std::vector<NetworkRequirement> requirements;
        /// Authoritative only: conditions that weaken the preset but do not stop the engine
        std::vector<std::string> warnings;

        /// True for a Deterministic project with an unmet requirement: Application::Init refuses to start
        [[nodiscard]] bool Refused() const;
        /// The unmet requirements, in order
        [[nodiscard]] std::vector<const NetworkRequirement *> Unmet() const;
    };

    /**
     * The startup checks of a preset (the S rows of the preset table):
     *   none           nothing is checked
     *   authoritative  warnings only: GPU physics on (a dedicated server runs the CPU scene), and a physics backend
     *                  that does not support set-state (it cannot correct a predicted entity)
     *   deterministic  every requirement below; any unmet one refuses the start (Refused). Requirements, in order:
     *                  physics-backend (the SELECTED backend declares CrossPlatform), physics-features (it implements
     *                  every feature group: layers and the collision matrix, triggers, contact and trigger events, scene
     *                  queries; the unmet text names the missing ones), strict-fp, simd-scalar (math.simdTier
     *                  is scalar), lua-seed, engine-trig, physics-threads (the backend's results do not depend on the
     *                  thread count: it declares at least SameBinary, which PhysX does only with 0 worker threads),
     *                  gpu-physics-off, fixed-timestep, tick-loop, and, only in rollback mode, snapshot (the backend
     *                  supports snapshot/restore)
     * A pure function of its arguments.
     */
    [[nodiscard]] NetworkValidation ValidateNetwork(const NetworkSettings &settings, const NetworkEnvironment &env);

    /// The application's simulation mode for a preset: none -> None (the offline loop), authoritative ->
    /// Authoritative, deterministic -> Deterministic. Application::Init applies it (SetSimulationMode) for a project
    /// that starts with a network preset; for none it changes nothing.
    [[nodiscard]] SimulationMode SimulationModeFor(NetworkPreset preset);

    /**
     * The engine health "Network" row: Disabled for none (and for a preset whose checks pass: no network runtime
     * exists yet, so the row never says Running), with the preset and, for authoritative, the warnings; Failed for a
     * refused Deterministic project, naming every unmet requirement. A generic SubsystemStatus: the editor protocol
     * is unchanged.
     */
    [[nodiscard]] SubsystemStatus MakeNetworkStatus(const NetworkSettings &settings,
                                                    const NetworkValidation &validation);
}
