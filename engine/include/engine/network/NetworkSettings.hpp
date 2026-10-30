#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace N2Engine::Network
{
    /// The project's network model (settings "network.preset"). None is the default and changes nothing about the
    /// engine. Authoritative and Deterministic only select a bundle of settings and startup checks today: there is no
    /// transport or replication yet.
    enum class NetworkPreset
    {
        None,
        Authoritative,
        Deterministic
    };

    /// network.deterministic.mode
    enum class DeterministicMode
    {
        Lockstep,
        Rollback
    };

    [[nodiscard]] constexpr std::string_view ToString(const NetworkPreset preset)
    {
        switch (preset)
        {
        case NetworkPreset::None:
            return "none";
        case NetworkPreset::Authoritative:
            return "authoritative";
        case NetworkPreset::Deterministic:
            return "deterministic";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view ToString(const DeterministicMode mode)
    {
        return mode == DeterministicMode::Rollback ? "rollback" : "lockstep";
    }

    /// "none", "authoritative" or "deterministic", case-insensitive; nullopt for anything else
    [[nodiscard]] std::optional<NetworkPreset> ParseNetworkPreset(std::string_view text);
    /// "lockstep" or "rollback", case-insensitive; nullopt for anything else
    [[nodiscard]] std::optional<DeterministicMode> ParseDeterministicMode(std::string_view text);

    struct TransportSettings
    {
        bool udp = true;
        bool webSocket = true;
        int port = 7777;
    };

    /// network.authoritative.timeDilation (netcode B6 follow-up): the client runs its fixed-step clock a little fast or
    /// slow to keep its input stamps at SuggestedInputTick. Opt-in (off by default). Not a numeric setting of the preset
    /// table (no NumericSettingSpec, no row): since editor protocol 1.23.0 the editor's Network panel shows it
    /// (GetNetworkSettings values.authoritative.timeDilation) and edits it (SetNetworkSettings accepts
    /// authoritative.timeDilation with enabled, minScale and maxScale); SetProjectSettings edits it too.
    struct TimeDilationSettings
    {
        bool enabled = false;
        /// Clamped to 0.5..1 and 1..2 when read
        double minScale = 0.9;
        double maxScale = 1.1;
    };

    struct AuthoritativeSettings
    {
        int snapshotRate = 30;
        int interpolationDelayMs = 100;
        int inputRedundancy = 4;
        int predictionMaxMs = 250;
        int smoothingMs = 100;
        double snapThreshold = 1.0;
        int lagCompMaxMs = 200;
        TimeDilationSettings timeDilation;
    };

    struct DeterministicSettings
    {
        DeterministicMode mode = DeterministicMode::Lockstep;
        int inputDelay = 2;
        int rollbackWindow = 8;
        int hashInterval = 30;
    };

    /// The project settings' "network" block, every value validated (ReadNetworkSettings in ProjectSettings.hpp).
    /// A default-constructed value is what a project without the block gets: preset none.
    struct NetworkSettings
    {
        NetworkPreset preset = NetworkPreset::None;
        int tickRate = 60;
        TransportSettings transport;
        AuthoritativeSettings authoritative;
        DeterministicSettings deterministic;
        /// Values the user changed away from the preset, by setting key (see NetworkPresets.hpp). Always an object.
        nlohmann::json overrides = nlohmann::json::object();
        /// The stored "customised" flag (the editor writes it). The preset table, not this flag, decides which
        /// guarantees an override loses (LostGuarantees); the flag is kept as it was written.
        bool customised = false;
    };

    /// The range of one numeric network setting: the one rule the settings parser and the preset table share
    struct NumericSettingSpec
    {
        std::string_view key; // "tickRate", "transport.port", "authoritative.snapshotRate", ...
        double min;
        double max;
        double defaultValue;
        bool integer;
    };

    /// Every numeric network setting
    [[nodiscard]] const std::vector<NumericSettingSpec> &NumericSettingSpecs();
    [[nodiscard]] const NumericSettingSpec *FindNumericSpec(std::string_view key);

    /// The deterministic.mode switch (netcode C5): the rollback window a session of these settings runs with
    /// (LockstepRuntimeOptions::rollbackWindow). deterministic.rollbackWindow, clamped to 0..15, for the Deterministic
    /// preset in rollback mode; 0 (lockstep) for lockstep mode and for every other preset.
    [[nodiscard]] std::uint32_t RollbackWindowFor(const NetworkSettings &settings) noexcept;
}
