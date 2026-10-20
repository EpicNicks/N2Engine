#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/NetworkSettings.hpp"

namespace N2Engine::Network
{
    /**
     * How a preset treats one setting (a cell of the preset table). A cell can be Locked and StartupChecked at once
     * ("L, S" in the design); a cell with no class bits is not governed by that preset.
     *
     *   Locked          the preset fixes the value; changing it marks the project Customised and loses the
     *                   guarantee the cell names (PresetCell::lostGuarantee)
     *   Tunable         the preset has a default; the project may change it within its range without losing anything
     *   StartupChecked  Application::Init checks it (ValidateNetwork): refuses to start a Deterministic project when
     *                   unmet, only warns for Authoritative
     */
    enum class SettingClass : std::uint8_t
    {
        Locked = 1,
        Tunable = 2,
        StartupChecked = 4
    };

    [[nodiscard]] constexpr std::uint8_t operator|(const SettingClass a, const SettingClass b)
    {
        return static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b);
    }

    [[nodiscard]] constexpr std::uint8_t operator|(const std::uint8_t a, const SettingClass b)
    {
        return a | static_cast<std::uint8_t>(b);
    }

    [[nodiscard]] constexpr bool HasClass(const std::uint8_t classes, const SettingClass cls)
    {
        return (classes & static_cast<std::uint8_t>(cls)) != 0;
    }

    /// One preset's treatment of one setting
    struct PresetCell
    {
        /// SettingClass bits; 0: the preset does not govern the setting (any value)
        std::uint8_t classes = 0;
        /// The project setting that carries the value ("math.simdTier", "network.tickRate", ...), empty when the cell
        /// is a requirement on the build or the backend rather than a setting. Overrides are keyed by it.
        std::string settingKey;
        /// The preset's value for the setting; null when the preset does not govern it, or when the cell is a
        /// requirement with no value to write (then `description` says it)
        nlohmann::json value;
        /// The design's wording for the cell (what the editor shows next to the value)
        std::string description;
        /// For a Locked cell: what changing it loses, shown in the Customised state
        std::string lostGuarantee;
    };

    /// One governed setting: its row of the design's preset table, with a cell per preset (index = NetworkPreset)
    struct PresetRow
    {
        std::string key;   // stable id, e.g. "physics.gpu"
        std::string label; // the design's row name
        std::array<PresetCell, 3> cells;

        [[nodiscard]] const PresetCell &Cell(const NetworkPreset preset) const
        {
            return cells[static_cast<std::size_t>(preset)];
        }
    };

    /// The preset table as data: the rows of the design's preset bundles plus the transport and runtime settings of
    /// the network block, in table order. The editor and the engine read this one table, so they apply the same rules.
    [[nodiscard]] const std::vector<PresetRow> &PresetTable();

    /// The row with this key, or nullptr
    [[nodiscard]] const PresetRow *FindPresetRow(std::string_view key);

    /// A guarantee an override loses
    struct LostGuarantee
    {
        std::string settingKey; // the overridden setting
        std::string rowKey;     // its row in the table
        std::string text;
    };

    /// The effective values of the settings `preset` governs: {settingKey: value} for every cell with a setting key
    /// and a value, with `overrides` ({settingKey: value}) applied on top. An override of a key the preset does not
    /// govern is ignored. For None the result is empty: the none preset governs nothing.
    /// A cell whose value follows the tick rate (physics.fixedTimestep is 1 / tickRate seconds) is resolved with
    /// `tickRate` to a number of seconds, the form the setting is written in.
    [[nodiscard]] nlohmann::json EffectiveValues(NetworkPreset preset,
                                                 const nlohmann::json &overrides = nlohmann::json::object(),
                                                 int tickRate = 60);

    /// The guarantees `preset` loses to `overrides`: one entry per overridden Locked setting whose override differs
    /// from the preset's value. Overriding a Tunable setting, or a Locked one to the preset's own value, loses nothing.
    /// A tick-rate-derived value (see EffectiveValues) matches an override within 0.1 percent.
    [[nodiscard]] std::vector<LostGuarantee> LostGuarantees(NetworkPreset preset, const nlohmann::json &overrides,
                                                            int tickRate = 60);

    /// Whether `overrides` leave the preset Customised (LostGuarantees is not empty)
    [[nodiscard]] bool IsCustomised(NetworkPreset preset, const nlohmann::json &overrides, int tickRate = 60);

    /// Whether `key` names a setting or a row of the table (for any preset)
    [[nodiscard]] bool IsKnownSettingKey(std::string_view key);
}
