#include "engine/network/NetworkValidation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace N2Engine::Network
{
    namespace
    {
        std::string SimdTierName(const Math::SIMDTierRequest request)
        {
            if (const auto level = Math::ToSIMDLevel(request))
            {
                return Math::SIMDLevelName(*level);
            }
            return "auto";
        }

        NetworkRequirement Requirement(std::string id, std::string text, const bool met, std::string unmetDetail)
        {
            return {std::move(id), std::move(text), met, met ? std::string{} : std::move(unmetDetail)};
        }
    }

    bool NetworkValidation::Refused() const
    {
        return preset == NetworkPreset::Deterministic && !Unmet().empty();
    }

    std::vector<const NetworkRequirement *> NetworkValidation::Unmet() const
    {
        std::vector<const NetworkRequirement *> unmet;
        for (const NetworkRequirement &requirement : requirements)
        {
            if (!requirement.met)
            {
                unmet.push_back(&requirement);
            }
        }
        return unmet;
    }

    NetworkValidation ValidateNetwork(const NetworkSettings &settings, const NetworkEnvironment &env)
    {
        NetworkValidation result;
        result.preset = settings.preset;

        if (settings.preset == NetworkPreset::Authoritative)
        {
            if (env.gpuMode != Physics::GpuMode::Off)
            {
                result.warnings.push_back(std::format(
                    "physics.gpu is {}: a dedicated server runs the CPU scene (it has no GPU, and GPU dynamics is "
                    "not reproducible), so keep it off there",
                    Physics::ToString(env.gpuMode)));
            }
            if (!env.hasBackend)
            {
                result.warnings.emplace_back(
                    "there is no physics backend: nothing can set the state of a predicted entity (SupportsSetState)");
            }
            else if (!env.backendSupportsSetState)
            {
                result.warnings.emplace_back(
                    "the physics backend does not support set-state (SupportsSetState is false): client "
                    "prediction cannot correct a physics body to the server state");
            }
            return result;
        }
        if (settings.preset != NetworkPreset::Deterministic)
        {
            return result;
        }

        auto &requirements = result.requirements;
        const std::string selected = env.backendName.empty()
                                         ? std::string("the backend")
                                         : std::format("the selected backend ({})", env.backendName);
        requirements.push_back(Requirement(
            "physics-backend", "the physics backend declares CrossPlatform determinism",
            env.hasBackend && env.backendLevel == Physics::DeterminismLevel::CrossPlatform,
            env.backendUnavailable
                ? std::format("physics.backend is {} but this build has no such backend", env.backendRequested)
            : env.hasBackend
                ? std::format("{} declares {}, and no backend declares CrossPlatform yet", selected,
                              Physics::ToString(env.backendLevel))
                : std::string("there is no physics backend")));
        requirements.push_back(Requirement(
            "strict-fp", "the build has the strict floating-point profile (N2ENGINE_STRICT_FP)", env.strictFp,
            "this build was not made with N2ENGINE_STRICT_FP"));
        requirements.push_back(Requirement(
            "simd-scalar", "math.simdTier is scalar", env.simdTier == Math::SIMDTierRequest::Scalar,
            std::format("math.simdTier is {}", SimdTierName(env.simdTier))));
        requirements.push_back(Requirement(
            "lua-seed", "determinism.luaSeed is set", env.luaSeedSet,
            "determinism.luaSeed is not set, so Lua string hash seed and math.random differ per run"));
        requirements.push_back(Requirement(
            "engine-trig", "engine-owned trig (C1)", env.engineOwnedTrig,
            "not built yet: math and Lua still call the C library sin, cos and atan2, which differ between "
            "platforms"));
        requirements.push_back(Requirement(
            "physics-threads", "physics results do not depend on the thread count",
            env.hasBackend && env.backendLevel >= Physics::DeterminismLevel::SameBinary,
            "the backend does not declare reproducible results (PhysX does with physics.deterministic, which "
            "runs it with 0 worker threads)"));
        requirements.push_back(Requirement(
            "gpu-physics-off", "physics.gpu is off", env.gpuMode == Physics::GpuMode::Off,
            std::format("physics.gpu is {}", Physics::ToString(env.gpuMode))));
        const double expectedStep = 1.0 / static_cast<double>(std::max(1, settings.tickRate));
        const bool stepMatches = env.fixedTimestepSeconds <= 0.0 ||
                                 std::abs(env.fixedTimestepSeconds - expectedStep) <= expectedStep * 1e-3;
        requirements.push_back(Requirement(
            "fixed-timestep", "the fixed timestep is on and its rate is network.tickRate",
            env.fixedTimestep && stepMatches,
            !env.fixedTimestep
                ? std::string("the fixed timestep is off")
                : std::format("physics.fixedTimestep is {} s but network.tickRate {} needs {} s",
                              env.fixedTimestepSeconds, settings.tickRate, expectedStep)));
        requirements.push_back(Requirement(
            "tick-loop",
            "the fixed-step loop is the netcode tick loop, independent of timeScale (a network SimulationMode)",
            env.ownedTickLoop,
            "the simulation runs in mode none: the offline loop drops time beyond MaxFrameTime and its fixed dt "
            "follows timeScale"));
        if (settings.deterministic.mode == DeterministicMode::Rollback)
        {
            requirements.push_back(Requirement(
                "snapshot", "the physics backend supports snapshot and restore (rollback)",
                env.hasBackend && env.backendSupportsSnapshot,
                env.hasBackend ? std::string("the backend reports SupportsSnapshot false")
                               : std::string("there is no physics backend")));
        }
        return result;
    }

    SimulationMode SimulationModeFor(const NetworkPreset preset)
    {
        switch (preset)
        {
        case NetworkPreset::Authoritative:
            return SimulationMode::Authoritative;
        case NetworkPreset::Deterministic:
            return SimulationMode::Deterministic;
        case NetworkPreset::None:
            break;
        }
        return SimulationMode::None;
    }

    SubsystemStatus MakeNetworkStatus(const NetworkSettings &settings, const NetworkValidation &validation)
    {
        SubsystemStatus status{.name = "Network"};
        if (validation.Refused())
        {
            const auto unmet = validation.Unmet();
            status.state = SubsystemState::Failed;
            status.detail = std::format("Preset deterministic: refused to start, {} unmet requirement{}: ",
                                        unmet.size(), unmet.size() == 1 ? "" : "s");
            bool first = true;
            for (const NetworkRequirement *requirement : unmet)
            {
                status.detail += std::format("{}{} ({})", first ? "" : "; ", requirement->text, requirement->detail);
                first = false;
            }
            return status;
        }
        status.state = SubsystemState::Disabled;
        if (settings.preset == NetworkPreset::None)
        {
            status.detail = "Preset none (offline): no network model";
            return status;
        }
        status.detail = std::format("Preset {}: settings validated, but the network runtime does not exist yet",
                                    ToString(settings.preset));
        for (const std::string &warning : validation.warnings)
        {
            status.detail += "; warning: " + warning;
        }
        return status;
    }
}
