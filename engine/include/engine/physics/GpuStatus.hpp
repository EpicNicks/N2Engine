#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/Health.hpp"

namespace N2Engine::Physics
{
    /// The project setting physics.gpu: whether the rigid-body scene may run on the GPU (PhysX GPU dynamics).
    /// Off is the default and makes no CUDA call at all.
    enum class GpuMode
    {
        Off,  // Never probe: the scene is the CPU scene on every machine
        Auto, // Probe; use the GPU scene when it works, else the CPU scene (Unavailable)
        On    // Probe; a failed probe is an error in the health row, but the CPU scene is still built
    };

    enum class GpuState
    {
        Disabled,    // physics.gpu is off (or there is no backend to ask)
        Unavailable, // Asked for (auto, or on in a build without a GPU library) but there is no usable GPU
        Ready,       // The scene runs with GPU dynamics
        Failed       // physics.gpu=on and the probe failed
    };

    struct GpuStatus
    {
        GpuState state = GpuState::Disabled;
        std::string detail;
    };

    [[nodiscard]] constexpr std::string_view ToString(const GpuMode mode)
    {
        switch (mode)
        {
        case GpuMode::Off:
            return "off";
        case GpuMode::Auto:
            return "auto";
        case GpuMode::On:
            return "on";
        }
        return "off";
    }

    [[nodiscard]] constexpr std::string_view ToString(const GpuState state)
    {
        switch (state)
        {
        case GpuState::Disabled:
            return "Disabled";
        case GpuState::Unavailable:
            return "Unavailable";
        case GpuState::Ready:
            return "Ready";
        case GpuState::Failed:
            return "Failed";
        }
        return "Disabled";
    }

    /// "off", "auto" or "on" (case-insensitive); nullopt for anything else
    [[nodiscard]] inline std::optional<GpuMode> ParseGpuMode(const std::string_view text)
    {
        std::string lower;
        lower.reserve(text.size());
        for (const char c : text)
        {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower == "off")
        {
            return GpuMode::Off;
        }
        if (lower == "auto")
        {
            return GpuMode::Auto;
        }
        if (lower == "on")
        {
            return GpuMode::On;
        }
        return std::nullopt;
    }

    /// A GPU feature's status: the scene's (the probe) unless the scene is Ready and the feature has failed, in
    /// which case the feature's own failure. Each feature (particles, soft bodies) keeps its own failure, so one
    /// feature failing never changes what another reports; the scene status itself is never rewritten by a feature.
    [[nodiscard]] inline GpuStatus ComposeFeatureGpuStatus(const GpuStatus &scene, const std::optional<GpuStatus> &failure)
    {
        if (scene.state == GpuState::Ready && failure.has_value())
        {
            return *failure;
        }
        return scene;
    }

    /// The soft-body status: ComposeFeatureGpuStatus, and while the scene is Ready but there is no GPU soft-body
    /// backend (`hasBackend` false: the scene is not a real GPU scene, or the backend was dropped) Unavailable, so
    /// the row never says Running for a feature that is not there.
    [[nodiscard]] inline GpuStatus ComposeSoftBodiesGpuStatus(const GpuStatus &scene, const std::optional<GpuStatus> &failure,
                                                              const bool hasBackend)
    {
        GpuStatus status = ComposeFeatureGpuStatus(scene, failure);
        if (status.state == GpuState::Ready && !hasBackend)
        {
            status = {GpuState::Unavailable, "GPU soft bodies unavailable: no PhysX soft-body backend in this build"};
        }
        return status;
    }

    /// A feature's health row for a GPU status. Disabled and Unavailable are both Disabled (deliberately off
    /// or not possible here: not a failure); only Ready is Running; only Failed (physics.gpu=on with a failed
    /// probe or feature) is Failed.
    [[nodiscard]] inline SubsystemStatus MakeGpuFeatureStatus(const std::string_view name, const GpuStatus &gpu)
    {
        SubsystemStatus status{.name = std::string(name)};
        switch (gpu.state)
        {
        case GpuState::Ready:
            status.state = SubsystemState::Running;
            break;
        case GpuState::Failed:
            status.state = SubsystemState::Failed;
            break;
        case GpuState::Disabled:
        case GpuState::Unavailable:
            status.state = SubsystemState::Disabled;
            break;
        }
        status.detail = gpu.detail;
        return status;
    }

    /// The "Particles" health row for a GPU status (see MakeGpuFeatureStatus)
    [[nodiscard]] inline SubsystemStatus MakeParticlesStatus(const GpuStatus &gpu)
    {
        return MakeGpuFeatureStatus("Particles", gpu);
    }

    /// The "SoftBodies" health row for a GPU status (see MakeGpuFeatureStatus)
    [[nodiscard]] inline SubsystemStatus MakeSoftBodiesStatus(const GpuStatus &gpu)
    {
        return MakeGpuFeatureStatus("SoftBodies", gpu);
    }
}
