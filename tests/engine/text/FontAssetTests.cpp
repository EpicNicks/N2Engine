#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>
#include <text/DefaultFont.hpp>

#include "engine/io/AssetMetadata.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/text/Font.hpp"

using namespace N2Engine;
using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Atlas settings from .meta customData
// ============================================================================

TEST(FontSettingsTest, MissingSettingsKeepTheDefaults)
{
    const Text::AtlasSettings defaults;
    for (const json &customData : {json(), json::object(), json{{"font", json::object()}}, json{{"other", 1}}})
    {
        const Text::AtlasSettings settings = Text::Font::ParseAtlasSettings(customData);
        EXPECT_EQ(settings.basePx, defaults.basePx);
        EXPECT_EQ(settings.spreadPx, defaults.spreadPx);
        EXPECT_EQ(settings.paddingPx, defaults.paddingPx);
        EXPECT_EQ(settings.charset, defaults.charset);
        EXPECT_TRUE(settings.extraChars.empty());
    }
}

TEST(FontSettingsTest, ReadsEveryKey)
{
    const json customData = {{"font", {{"basePx", 32}, {"spreadPx", 6}, {"paddingPx", 3}, {"charset", "ascii"},
                                       {"extraChars", "\xE2\x82\xAC\xC3\xA9"}}}};
    const Text::AtlasSettings settings = Text::Font::ParseAtlasSettings(customData);
    EXPECT_EQ(settings.basePx, 32.0f);
    EXPECT_EQ(settings.spreadPx, 6);
    EXPECT_EQ(settings.paddingPx, 3);
    EXPECT_EQ(settings.charset, Text::Charset::Ascii);
    EXPECT_EQ(settings.extraChars, (std::u32string{0x20AC, 0xE9}));

    const json latin1 = {{"font", {{"charset", "latin1"}}}};
    Text::AtlasSettings ascii;
    ascii.charset = Text::Charset::Ascii;
    EXPECT_EQ(Text::Font::ParseAtlasSettings(latin1, ascii).charset, Text::Charset::Latin1);
}

TEST(FontSettingsTest, BadValuesAreIgnored)
{
    const json customData = {{"font", {{"basePx", "big"}, {"spreadPx", 1000}, {"paddingPx", -1}, {"charset", "klingon"},
                                       {"extraChars", 5}}}};
    const Text::AtlasSettings defaults;
    const Text::AtlasSettings settings = Text::Font::ParseAtlasSettings(customData);
    EXPECT_EQ(settings.basePx, defaults.basePx);
    EXPECT_EQ(settings.spreadPx, defaults.spreadPx);
    EXPECT_EQ(settings.paddingPx, defaults.paddingPx);
    EXPECT_EQ(settings.charset, defaults.charset);
    EXPECT_TRUE(settings.extraChars.empty());

    EXPECT_EQ(Text::Font::ParseAtlasSettings(json{{"font", "not an object"}}).basePx, defaults.basePx);
    EXPECT_EQ(Text::Font::ParseAtlasSettings(json{{"font", {{"basePx", 2}}}}).basePx, defaults.basePx);
    // Above the cost cap
    EXPECT_EQ(Text::Font::ParseAtlasSettings(json{{"font", {{"basePx", 512}}}}).basePx, defaults.basePx);
}

TEST(FontSettingsTest, ASpreadTooLargeForTheBaseSizeIsReduced)
{
    const Text::AtlasSettings settings = Text::Font::ParseAtlasSettings(json{{"font", {{"basePx", 16}, {"spreadPx", 20}}}});
    EXPECT_EQ(settings.basePx, 16.0f);
    EXPECT_EQ(settings.spreadPx, Text::MaxSpreadPx(16.0f));
}

// ============================================================================
// The built-in font
// ============================================================================

TEST(DefaultFontAssetTest, LoadsFromTheEmbeddedBytes)
{
    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    ASSERT_TRUE(font->IsLoaded());
    EXPECT_EQ(font->GetResourceType(), "Font");
    EXPECT_EQ(font->GetAtlasSettings().basePx, Text::AtlasSettings{}.basePx);
    EXPECT_EQ(font->GetAtlasSettings().charset, Text::Charset::Latin1);
    EXPECT_EQ(Text::Font::GetDefault(), font) << "built once and shared";

    const Text::TextLayout layout = font->Layout("Hello, world");
    EXPECT_EQ(layout.quads.size(), 11u);
    EXPECT_GT(layout.bounds.Width(), 0.0f);
}

TEST(DefaultFontAssetTest, UnreadableDataGivesNoFont)
{
    const std::string notAFont = "definitely not a font file";
    EXPECT_EQ(Text::Font::CreateFromMemory(std::as_bytes(std::span(notAFont)), {}, "bad"), nullptr);

    const Text::Font unloaded{};
    EXPECT_FALSE(unloaded.IsLoaded());
    EXPECT_TRUE(unloaded.Layout("text").quads.empty());
    EXPECT_EQ(unloaded.GetAtlasSettings().basePx, Text::AtlasSettings{}.basePx) << "defaults, not a null dereference";
}

// ============================================================================
// Fonts as project assets
// ============================================================================

class FontAssetTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;
    static inline const IO::ResourcePath kFontPath{"res://fonts/body.ttf"};

    static fs::path MetaPath() { return s_root / ".import" / "fonts" / "body.ttf.meta"; }

    void SetUp() override
    {
        // No explicit Font::RegisterLoader(): ResourceLoader::Initialize registers the font loader itself
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_font_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "fonts");

        const auto bytes = Text::GetDefaultFontData();
        std::ofstream(s_root / "assets" / "fonts" / "body.ttf", std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "FontAssetTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    // Writes settings into the font's .meta and rescans, as editing it in the editor and reopening would
    static void SetCustomData(const json &customData)
    {
        IO::AssetMetadata meta = IO::AssetMetadata::FromFile(MetaPath());
        meta.customData = customData;
        ASSERT_TRUE(meta.SaveToFile(MetaPath()));
        IO::ResourceLoader::Instance().Initialize(s_root);
    }
};

TEST_F(FontAssetTest, FontFilesAreScannedAsFontAssets)
{
    auto &loader = IO::ResourceLoader::Instance();
    const IO::AssetMetadata *meta = loader.GetMetadata(kFontPath);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->resourceType, "Font");
    EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(kFontPath));
    EXPECT_EQ(loader.GetAssetsByType("Font").size(), 1u);
    EXPECT_TRUE(fs::exists(MetaPath()));
}

TEST_F(FontAssetTest, LoadsWithTheSettingsFromItsMeta)
{
    SetCustomData({{"font", {{"basePx", 20}, {"spreadPx", 3}, {"charset", "ascii"}, {"extraChars", "\xC3\xA9"}}}});

    const auto font = IO::ResourceLoader::Instance().Load<Text::Font>(kFontPath);
    ASSERT_NE(font, nullptr);
    ASSERT_TRUE(font->IsLoaded());
    EXPECT_EQ(font->GetUUID(), IO::ResourceLoader::Instance().GetUUID(kFontPath));

    const Text::AtlasSettings &settings = font->GetAtlasSettings();
    EXPECT_EQ(settings.basePx, 20.0f);
    EXPECT_EQ(settings.spreadPx, 3);
    EXPECT_EQ(settings.charset, Text::Charset::Ascii);

    const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
    EXPECT_NE(atlas.FindCodepoint(U'A'), nullptr);
    EXPECT_NE(atlas.FindCodepoint(0xE9), nullptr) << "extraChars";
    EXPECT_EQ(atlas.FindCodepoint(0xF1), nullptr) << "Latin-1 but not in the ASCII charset";
}

TEST_F(FontAssetTest, LoadsThroughResourcesWithItsMetaUuid)
{
    SetCustomData({{"font", {{"basePx", 16}, {"spreadPx", 2}, {"charset", "ascii"}}}});

    const auto font = IO::Resources::Instance().Load<Text::Font>("res://fonts/body.ttf");
    ASSERT_NE(font, nullptr);
    EXPECT_EQ(font->GetUUID(), IO::ResourceLoader::Instance().GetUUID(kFontPath));
    EXPECT_EQ(IO::ResourceLoader::Instance().GetCached<Text::Font>(kFontPath), font);
    EXPECT_EQ(font->GetAtlasSettings().basePx, 16.0f);
}

TEST_F(FontAssetTest, ACorruptFontFailsToLoad)
{
    std::ofstream(s_root / "assets" / "fonts" / "broken.otf", std::ios::binary) << "not a font";
    IO::ResourceLoader::Instance().RescanAssets();

    const IO::ResourcePath path("res://fonts/broken.otf");
    ASSERT_TRUE(IO::ResourceLoader::Instance().Exists(path));
    EXPECT_EQ(IO::ResourceLoader::Instance().GetMetadata(path)->resourceType, "Font");
    EXPECT_EQ(IO::ResourceLoader::Instance().Load<Text::Font>(path), nullptr);
}
