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

    /// The "Particles" health row for a GPU status. Disabled and Unavailable are both Disabled (deliberately off
    /// or not possible here: not a failure); only Ready is Running; only Failed (physics.gpu=on with a failed
    /// probe) is Failed.
    [[nodiscard]] inline SubsystemStatus MakeParticlesStatus(const GpuStatus &gpu)
    {
        SubsystemStatus status{.name = "Particles"};
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
}
