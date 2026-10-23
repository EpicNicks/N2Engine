#pragma once

#include <string_view>

namespace N2Engine::Physics
{
    /// What a physics backend promises about reproducing its results (IPhysicsBackend::GetDeterminism). Ordered: a
    /// larger value promises everything a smaller one does.
    enum class DeterminismLevel
    {
        None,          // No promise: the same input may give different results (the default for any backend)
        SameBinary,    // Bit-identical results from one binary on one platform, run to run (replays, tests)
        CrossPlatform  // Bit-identical results on every supported platform and compiler: what lockstep and rollback need
    };

    [[nodiscard]] constexpr std::string_view ToString(const DeterminismLevel level)
    {
        switch (level)
        {
        case DeterminismLevel::None:
            return "None";
        case DeterminismLevel::SameBinary:
            return "SameBinary";
        case DeterminismLevel::CrossPlatform:
            return "CrossPlatform";
        }
        return "Unknown";
    }
}
