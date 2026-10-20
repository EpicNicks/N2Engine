#include "engine/network/NetworkSettings.hpp"

#include <algorithm>
#include <cctype>

namespace N2Engine::Network
{
    namespace
    {
        bool EqualsIgnoreCase(const std::string_view a, const std::string_view b)
        {
            return std::ranges::equal(a, b, [](const unsigned char x, const unsigned char y)
            {
                return std::tolower(x) == std::tolower(y);
            });
        }
    }

    std::optional<NetworkPreset> ParseNetworkPreset(const std::string_view text)
    {
        for (const NetworkPreset preset : {NetworkPreset::None, NetworkPreset::Authoritative,
                                           NetworkPreset::Deterministic})
        {
            if (EqualsIgnoreCase(text, ToString(preset)))
            {
                return preset;
            }
        }
        return std::nullopt;
    }

    std::optional<DeterministicMode> ParseDeterministicMode(const std::string_view text)
    {
        for (const DeterministicMode mode : {DeterministicMode::Lockstep, DeterministicMode::Rollback})
        {
            if (EqualsIgnoreCase(text, ToString(mode)))
            {
                return mode;
            }
        }
        return std::nullopt;
    }

    const std::vector<NumericSettingSpec> &NumericSettingSpecs()
    {
        // The defaults are the design's (netcode-runtime-design 5.1); the ranges are its tunable ranges where it gives
        // one (tick rate 30-120 Hz, input delay 0-6 ticks, rollback window 0-15 ticks, prediction at most 250 ms) and
        // otherwise a sanity bound
        static const std::vector<NumericSettingSpec> specs = {
            {"tickRate", 30, 120, 60, true},
            {"transport.port", 1, 65535, 7777, true},
            {"authoritative.snapshotRate", 10, 60, 30, true},
            {"authoritative.interpolationDelayMs", 0, 1000, 100, true},
            {"authoritative.inputRedundancy", 1, 8, 4, true},
            {"authoritative.predictionMaxMs", 0, 250, 250, true},
            {"authoritative.smoothingMs", 0, 1000, 100, true},
            {"authoritative.snapThreshold", 0.01, 100.0, 1.0, false},
            {"authoritative.lagCompMaxMs", 0, 1000, 200, true},
            {"deterministic.inputDelay", 0, 6, 2, true},
            {"deterministic.rollbackWindow", 0, 15, 8, true},
            {"deterministic.hashInterval", 1, 600, 30, true},
        };
        return specs;
    }

    const NumericSettingSpec *FindNumericSpec(const std::string_view key)
    {
        for (const NumericSettingSpec &spec : NumericSettingSpecs())
        {
            if (spec.key == key)
            {
                return &spec;
            }
        }
        return nullptr;
    }
}
