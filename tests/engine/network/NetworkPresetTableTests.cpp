#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/NetworkPresets.hpp"
#include "engine/network/NetworkSettings.hpp"

// The preset table as data: the rows of the design's preset bundles (L locked / T tunable / S startup-checked), the
// effective values of a preset, and the guarantees an override loses ("Customised").

using namespace N2Engine::Network;
using nlohmann::json;

namespace
{
    constexpr NetworkPreset None = NetworkPreset::None;
    constexpr NetworkPreset Authoritative = NetworkPreset::Authoritative;
    constexpr NetworkPreset Deterministic = NetworkPreset::Deterministic;

    const PresetCell &CellOf(const char *rowKey, const NetworkPreset preset)
    {
        const PresetRow *row = FindPresetRow(rowKey);
        EXPECT_NE(row, nullptr) << rowKey;
        static const PresetCell empty{};
        return row != nullptr ? row->Cell(preset) : empty;
    }

    bool Has(const std::uint8_t classes, const SettingClass cls)
    {
        return HasClass(classes, cls);
    }
}

TEST(NetworkPresetTableTest, HasTheDesignRowsWithOneCellPerPreset)
{
    const std::vector<PresetRow> &table = PresetTable();
    // 18 rows of the preset bundles plus the 8 transport and runtime settings the bundles do not cover
    EXPECT_EQ(table.size(), 26u);
    std::set<std::string> keys;
    for (const PresetRow &row : table)
    {
        EXPECT_FALSE(row.key.empty());
        EXPECT_FALSE(row.label.empty()) << row.key;
        EXPECT_TRUE(keys.insert(row.key).second) << "duplicate row " << row.key;
        for (const PresetCell &cell : row.cells)
        {
            EXPECT_FALSE(cell.description.empty()) << row.key;
        }
    }
    EXPECT_EQ(FindPresetRow("no.such.row"), nullptr);
}

TEST(NetworkPresetTableTest, TheNonePresetGovernsNothing)
{
    for (const PresetRow &row : PresetTable())
    {
        const PresetCell &cell = row.Cell(None);
        EXPECT_EQ(cell.classes, 0) << row.key;
        EXPECT_TRUE(cell.value.is_null()) << row.key;
        EXPECT_TRUE(cell.settingKey.empty()) << row.key;
    }
    EXPECT_TRUE(EffectiveValues(None).empty());
    EXPECT_TRUE(EffectiveValues(None, json{{"physics.gpu", "on"}}).empty());
    EXPECT_TRUE(LostGuarantees(None, json{{"physics.gpu", "on"}, {"math.simdTier", "auto"}}).empty());
    EXPECT_FALSE(IsCustomised(None, json{{"physics.gpu", "on"}}));
}

TEST(NetworkPresetTableTest, DeterministicRowsAreLockedAndStartupChecked)
{
    const PresetCell &gpu = CellOf("physics.gpu", Deterministic);
    EXPECT_TRUE(Has(gpu.classes, SettingClass::Locked));
    EXPECT_TRUE(Has(gpu.classes, SettingClass::StartupChecked));
    EXPECT_FALSE(Has(gpu.classes, SettingClass::Tunable));
    EXPECT_EQ(gpu.settingKey, "physics.gpu");
    EXPECT_EQ(gpu.value, "off");

    const PresetCell &simd = CellOf("math.simdTier", Deterministic);
    EXPECT_TRUE(Has(simd.classes, SettingClass::Locked));
    EXPECT_EQ(simd.settingKey, "math.simdTier");
    EXPECT_EQ(simd.value, "scalar");

    const PresetCell &backend = CellOf("physics.backend", Deterministic);
    EXPECT_TRUE(Has(backend.classes, SettingClass::Locked));
    EXPECT_TRUE(Has(backend.classes, SettingClass::StartupChecked));
    // The preset locks the setting itself to jolt (the only backend that can declare CrossPlatform); the L/T/S class stays
    EXPECT_EQ(backend.settingKey, "physics.backend");
    EXPECT_EQ(backend.value, "jolt");
    EXPECT_EQ(EffectiveValues(Deterministic).at("physics.backend"), "jolt");
    EXPECT_FALSE(EffectiveValues(Authoritative).contains("physics.backend"));
    EXPECT_TRUE(IsKnownSettingKey("physics.backend"));
    // Choosing physx on a Deterministic project loses the guarantee; choosing jolt (any case) does not
    EXPECT_FALSE(LostGuarantees(Deterministic, json{{"physics.backend", "physx"}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"physics.backend", "jolt"}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"physics.backend", "JOLT"}}).empty());

    // The rows the design marks S for the Deterministic preset, and no others
    std::set<std::string> startupChecked;
    for (const PresetRow &row : PresetTable())
    {
        if (Has(row.Cell(Deterministic).classes, SettingClass::StartupChecked))
        {
            startupChecked.insert(row.key);
        }
    }
    EXPECT_EQ(startupChecked, (std::set<std::string>{
                                  "physics.backend", "physics.gpu", "physics.fixedTimestep", "simulation.tickDriver",
                                  "math.simdTier", "build.strictFp", "math.transcendentals", "physics.snapshot",
                                  "random.policy", "ordering.rules", "physics.threads"}));
}

TEST(NetworkPresetTableTest, EveryLockedCellNamesTheGuaranteeItLoses)
{
    for (const PresetRow &row : PresetTable())
    {
        for (const PresetCell &cell : row.cells)
        {
            if (Has(cell.classes, SettingClass::Locked))
            {
                EXPECT_FALSE(cell.lostGuarantee.empty()) << row.key;
            }
        }
    }
}

TEST(NetworkPresetTableTest, AuthoritativeRowsAreTunableOrWarnOnly)
{
    const PresetCell &snapshotRate = CellOf("state.sync", Authoritative);
    EXPECT_TRUE(Has(snapshotRate.classes, SettingClass::Tunable));
    EXPECT_EQ(snapshotRate.settingKey, "network.authoritative.snapshotRate");
    EXPECT_EQ(snapshotRate.value, 30);

    // The set-state row is checked at startup, with a warning (never a refusal)
    EXPECT_TRUE(Has(CellOf("physics.snapshot", Authoritative).classes, SettingClass::StartupChecked));
    // Nothing is locked in the Authoritative preset
    for (const PresetRow &row : PresetTable())
    {
        EXPECT_FALSE(Has(row.Cell(Authoritative).classes, SettingClass::Locked)) << row.key;
    }
}

TEST(NetworkPresetTableTest, NumericDefaultsAgreeWithTheSettingsDefaults)
{
    const NetworkSettings defaults;
    EXPECT_EQ(CellOf("network.tickRate", Authoritative).value, defaults.tickRate);
    EXPECT_EQ(CellOf("network.transport.port", Deterministic).value, defaults.transport.port);
    EXPECT_EQ(CellOf("network.authoritative.lagCompMaxMs", Authoritative).value, defaults.authoritative.lagCompMaxMs);
    EXPECT_EQ(CellOf("network.authoritative.snapThreshold", Authoritative).value,
              defaults.authoritative.snapThreshold);
    EXPECT_EQ(CellOf("deterministic.inputDelay", Deterministic).value, defaults.deterministic.inputDelay);
    EXPECT_EQ(CellOf("prediction.window", Deterministic).value, defaults.deterministic.rollbackWindow);
    EXPECT_EQ(CellOf("prediction.window", Authoritative).value, defaults.authoritative.predictionMaxMs);
    EXPECT_EQ(CellOf("network.deterministic.mode", Deterministic).value, "lockstep");
    EXPECT_EQ(CellOf("network.transport.udp", Authoritative).value, defaults.transport.udp);
}

TEST(NetworkPresetTableTest, EffectiveValuesAreThePresetsSettings)
{
    const json deterministic = EffectiveValues(Deterministic);
    EXPECT_EQ(deterministic.at("physics.gpu"), "off");
    EXPECT_EQ(deterministic.at("math.simdTier"), "scalar");
    EXPECT_EQ(deterministic.at("network.tickRate"), 60);
    EXPECT_EQ(deterministic.at("network.deterministic.inputDelay"), 2);
    EXPECT_EQ(deterministic.at("network.deterministic.rollbackWindow"), 8);
    EXPECT_EQ(deterministic.at("network.deterministic.hashInterval"), 30);
    EXPECT_FALSE(deterministic.contains("network.authoritative.snapshotRate"));
    // A requirement with no value to write is not a setting
    EXPECT_FALSE(deterministic.contains("build.strictFp"));

    const json authoritative = EffectiveValues(Authoritative);
    EXPECT_EQ(authoritative.at("network.authoritative.snapshotRate"), 30);
    EXPECT_EQ(authoritative.at("network.authoritative.predictionMaxMs"), 250);
    EXPECT_EQ(authoritative.at("physics.gpu"), "off");
    EXPECT_FALSE(authoritative.contains("math.simdTier"));
    EXPECT_FALSE(authoritative.contains("network.deterministic.inputDelay"));
}

TEST(NetworkPresetTableTest, TheFixedTimestepIsSecondsDerivedFromTheTickRate)
{
    const json at60 = EffectiveValues(Deterministic);
    ASSERT_TRUE(at60.at("physics.fixedTimestep").is_number());
    EXPECT_DOUBLE_EQ(at60.at("physics.fixedTimestep").get<double>(), 1.0 / 60.0);
    EXPECT_DOUBLE_EQ(EffectiveValues(Deterministic, json::object(), 120).at("physics.fixedTimestep").get<double>(),
                     1.0 / 120.0);
    // Exactly 1 / tickRate (as a project file rounds it) loses nothing; another rate does
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"physics.fixedTimestep", 0.0166667}}, 60).empty());
    EXPECT_FALSE(IsCustomised(Deterministic, json{{"physics.fixedTimestep", 1.0 / 30.0}}, 30));
    EXPECT_TRUE(IsCustomised(Deterministic, json{{"physics.fixedTimestep", 0.0166667}}, 30));
}

TEST(NetworkPresetTableTest, OverridesChangeOnlyTheSettingsThePresetGoverns)
{
    const json overrides = {{"math.simdTier", "auto"},
                            {"network.tickRate", 30},
                            {"network.authoritative.snapshotRate", 20},
                            {"nonsense", 1}};
    const json effective = EffectiveValues(Deterministic, overrides);
    EXPECT_EQ(effective.at("math.simdTier"), "auto");
    EXPECT_EQ(effective.at("network.tickRate"), 30);
    EXPECT_FALSE(effective.contains("network.authoritative.snapshotRate"));
    EXPECT_FALSE(effective.contains("nonsense"));
    // Not an object: ignored
    EXPECT_EQ(EffectiveValues(Deterministic, json::array()), EffectiveValues(Deterministic));
}

TEST(NetworkPresetTableTest, OverridingALockedSettingMarksItCustomisedAndListsTheLostGuarantee)
{
    const auto lost = LostGuarantees(Deterministic, json{{"math.simdTier", "auto"}});
    ASSERT_EQ(lost.size(), 1u);
    EXPECT_EQ(lost[0].settingKey, "math.simdTier");
    EXPECT_EQ(lost[0].rowKey, "math.simdTier");
    EXPECT_EQ(lost[0].text, "SIMD tier Auto: x86 and ARM peers will desync");
    EXPECT_TRUE(IsCustomised(Deterministic, json{{"math.simdTier", "auto"}}));

    const auto timestep = LostGuarantees(Deterministic, json{{"physics.fixedTimestep", 0.02}});
    ASSERT_EQ(timestep.size(), 1u);
    EXPECT_NE(timestep[0].text.find("timeScale"), std::string::npos);
}

TEST(NetworkPresetTableTest, ChangesThatLoseNothing)
{
    // The preset's own value (any case), a tunable value, and a tunable-with-locked-hash rate
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"math.simdTier", "scalar"}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"math.simdTier", "SCALAR"}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"physics.gpu", "off"}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"network.tickRate", 30}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"network.deterministic.hashInterval", 120}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json{{"nonsense", 1}}).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json::object()).empty());
    EXPECT_TRUE(LostGuarantees(Deterministic, json::array()).empty());
    EXPECT_FALSE(IsCustomised(Deterministic, json::object()));
    // Authoritative locks nothing, so no override customises it
    EXPECT_TRUE(LostGuarantees(Authoritative, json{{"physics.gpu", "on"}, {"network.tickRate", 120}}).empty());
}

TEST(NetworkPresetTableTest, SeveralOverridesListEveryLostGuaranteeInTableOrder)
{
    const json overrides = {{"build.strictFp", false}, {"math.simdTier", "avx"}, {"physics.gpu", "on"}};
    const auto lost = LostGuarantees(Deterministic, overrides);
    ASSERT_EQ(lost.size(), 3u);
    EXPECT_EQ(lost[0].rowKey, "physics.gpu");
    EXPECT_EQ(lost[1].rowKey, "math.simdTier");
    EXPECT_EQ(lost[2].rowKey, "build.strictFp");
    // A requirement without a value is keyed by its row, and any override of it waives it
    EXPECT_EQ(lost[2].settingKey, "build.strictFp");
    EXPECT_TRUE(IsCustomised(Deterministic, json{{"physics.backend", true}}));
}

TEST(NetworkPresetTableTest, KnownSettingKeysAreRowsAndSettings)
{
    EXPECT_TRUE(IsKnownSettingKey("physics.gpu"));
    EXPECT_TRUE(IsKnownSettingKey("math.simdTier"));
    EXPECT_TRUE(IsKnownSettingKey("build.strictFp"));
    EXPECT_TRUE(IsKnownSettingKey("network.authoritative.snapshotRate"));
    EXPECT_TRUE(IsKnownSettingKey("network.deterministic.rollbackWindow"));
    EXPECT_FALSE(IsKnownSettingKey("nonsense"));
    EXPECT_FALSE(IsKnownSettingKey(""));
}
