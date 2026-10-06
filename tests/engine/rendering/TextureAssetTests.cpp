#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>
#include <renderer/common/TextureOptions.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/io/AssetMetadata.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Image.hpp"

#include "TextureTestSupport.hpp"

// The Texture asset: settings from .meta customData, Texture::Create, decoding, and image files as project
// assets (types, case-insensitive extensions, stable UUIDs), plus an Image's sprite through a scene save and
// load

using namespace N2Engine;
using Rendering::Texture;
using Rendering::TextureSettings;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using TextureTestSupport::MakeBmp;
using TextureTestSupport::Rgb;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    using Rgba = std::array<std::uint8_t, 4>;

    Rgba PixelAt(const Texture &texture, const std::uint32_t x, const std::uint32_t row)
    {
        const std::size_t i = (static_cast<std::size_t>(row) * texture.GetWidth() + x) * 4;
        const auto &p = texture.GetPixels();
        return Rgba{p[i], p[i + 1], p[i + 2], p[i + 3]};
    }

    // A 2 x 2 picture, top row first: red, green over blue, white
    constexpr Rgb kRed{255, 0, 0};
    constexpr Rgb kGreen{0, 255, 0};
    constexpr Rgb kBlue{0, 0, 255};
    constexpr Rgb kWhite{255, 255, 255};

    std::vector<std::uint8_t> CheckerBmp()
    {
        return MakeBmp(2, 2, {kRed, kGreen, kBlue, kWhite});
    }

    /// Collects the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        std::vector<std::string> warnings;

    private:
        size_t _id = 0;
    };
}

// ============================================================================
// Settings from .meta customData
// ============================================================================

TEST(TextureSettingsTest, MissingSettingsKeepTheDefaults)
{
    const TextureSettings defaults;
    EXPECT_TRUE(defaults.srgb);
    EXPECT_EQ(defaults.filter, TextureFilter::Linear);
    EXPECT_EQ(defaults.wrap, TextureWrap::Repeat);
    EXPECT_TRUE(defaults.mipmaps);
    EXPECT_TRUE(defaults.flipY);
    EXPECT_EQ(defaults.ToTextureOptions(), TextureOptions::Default()) << "the renderer defaults";

    WarningCapture capture;
    for (const json &customData : {json(), json::object(), json{{"texture", json::object()}}, json{{"font", {{"basePx", 20}}}}})
    {
        EXPECT_EQ(Texture::ParseSettings(customData), defaults) << customData.dump();
    }
    EXPECT_TRUE(capture.warnings.empty()) << "nothing to warn about";
}

TEST(TextureSettingsTest, ReadsEveryKey)
{
    const json customData = {{"texture", {{"srgb", false}, {"filter", "nearest"}, {"wrap", "clamp"}, {"mipmaps", false},
                                          {"flipY", false}}}};
    const TextureSettings settings = Texture::ParseSettings(customData);
    EXPECT_FALSE(settings.srgb);
    EXPECT_EQ(settings.filter, TextureFilter::Nearest);
    EXPECT_EQ(settings.wrap, TextureWrap::ClampToEdge);
    EXPECT_FALSE(settings.mipmaps);
    EXPECT_FALSE(settings.flipY);

    const TextureOptions options = settings.ToTextureOptions();
    EXPECT_EQ(options.filter, TextureFilter::Nearest);
    EXPECT_EQ(options.wrap, TextureWrap::ClampToEdge);
    EXPECT_FALSE(options.mipmaps);

    // The other values, on top of non-default defaults
    const TextureSettings back = Texture::ParseSettings(json{{"texture", {{"filter", "linear"}, {"wrap", "repeat"}}}},
                                                        settings);
    EXPECT_EQ(back.filter, TextureFilter::Linear);
    EXPECT_EQ(back.wrap, TextureWrap::Repeat);
    EXPECT_FALSE(back.flipY) << "unset keys keep the defaults passed in";
}

TEST(TextureSettingsTest, BadValuesAreIgnoredWithAWarningEach)
{
    WarningCapture capture;
    const json customData = {{"texture", {{"srgb", "yes"}, {"filter", "cubic"}, {"wrap", 3}, {"mipmaps", nullptr},
                                          {"flipY", 1}}}};
    EXPECT_EQ(Texture::ParseSettings(customData), TextureSettings{});
    ASSERT_EQ(capture.warnings.size(), 5u);
    for (const std::string_view key : {"srgb", "filter", "wrap", "mipmaps", "flipY"})
    {
        const bool named = std::ranges::any_of(capture.warnings, [key](const std::string &warning)
        {
            return warning.starts_with("Texture settings") && warning.find(std::string("\"") + std::string(key) + "\"") != std::string::npos;
        });
        EXPECT_TRUE(named) << "a warning naming " << key;
    }

    capture.warnings.clear();
    EXPECT_EQ(Texture::ParseSettings(json{{"texture", "not an object"}}), TextureSettings{});
    EXPECT_EQ(capture.warnings.size(), 1u);
}

// ============================================================================
// Runtime and decoded textures
// ============================================================================

TEST(TextureCreateTest, MakesATextureFromRgbaPixels)
{
    const std::vector<std::uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    TextureOptions options;
    options.filter = TextureFilter::Nearest;
    options.wrap = TextureWrap::ClampToEdge;
    options.mipmaps = false;

    const auto texture = Texture::Create(3, 1, pixels, options);
    ASSERT_NE(texture, nullptr);
    EXPECT_TRUE(texture->IsLoaded());
    EXPECT_EQ(texture->GetResourceType(), "Texture");
    EXPECT_EQ(texture->GetWidth(), 3u);
    EXPECT_EQ(texture->GetHeight(), 1u);
    EXPECT_EQ(Texture::GetChannels(), 4u);
    EXPECT_EQ(texture->GetPixels(), pixels);
    EXPECT_EQ(texture->GetTextureOptions(), options);
    EXPECT_TRUE(texture->GetSourcePath().empty());
    EXPECT_NE(texture->GetUUID(), Math::UUID::ZERO);

    const auto defaults = Texture::Create(1, 1, std::vector<std::uint8_t>{0, 0, 0, 255});
    ASSERT_NE(defaults, nullptr);
    EXPECT_EQ(defaults->GetTextureOptions(), TextureOptions::Default());
    EXPECT_NE(defaults->GetUUID(), texture->GetUUID());
}

TEST(TextureCreateTest, AWrongSizeGivesNoTexture)
{
    const std::vector<std::uint8_t> pixels(12, 0);
    EXPECT_EQ(Texture::Create(2, 2, pixels), nullptr) << "2 x 2 needs 16 bytes";
    EXPECT_EQ(Texture::Create(0, 3, pixels), nullptr);
    EXPECT_EQ(Texture::Create(3, 0, std::vector<std::uint8_t>{}), nullptr);

    const Texture unloaded;
    EXPECT_FALSE(unloaded.IsLoaded());
    EXPECT_TRUE(unloaded.GetPixels().empty());
}

TEST(TextureCreateTest, DecodesAnImageFileUprightByDefault)
{
    const auto bmp = CheckerBmp();
    const auto texture = Texture::CreateFromEncoded(bmp);
    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetWidth(), 2u);
    ASSERT_EQ(texture->GetHeight(), 2u);
    // flipY: row 0 (v = 0) is the bottom of the picture
    EXPECT_EQ(PixelAt(*texture, 0, 0), (Rgba{0, 0, 255, 255}));
    EXPECT_EQ(PixelAt(*texture, 1, 0), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(PixelAt(*texture, 0, 1), (Rgba{255, 0, 0, 255}));
    EXPECT_EQ(PixelAt(*texture, 1, 1), (Rgba{0, 255, 0, 255}));

    TextureSettings topFirst;
    topFirst.flipY = false;
    const auto unflipped = Texture::CreateFromEncoded(bmp, topFirst);
    ASSERT_NE(unflipped, nullptr);
    EXPECT_EQ(PixelAt(*unflipped, 0, 0), (Rgba{255, 0, 0, 255})) << "without flipY the top row comes first";
    EXPECT_FALSE(unflipped->GetSettings().flipY);

    const std::string notAnImage = "not an image at all";
    EXPECT_EQ(Texture::CreateFromEncoded(std::span(reinterpret_cast<const std::uint8_t *>(notAnImage.data()),
                                                   notAnImage.size())),
              nullptr);
}

// ============================================================================
// Image files as project assets
// ============================================================================

class TextureAssetTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;
    static inline const IO::ResourcePath kCheckerPath{"res://textures/checker.bmp"};

    static fs::path MetaPath(const std::string &relative) { return s_root / ".import" / (relative + ".meta"); }

    static void WriteFile(const std::string &relative, const std::vector<std::uint8_t> &bytes)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void SetUp() override
    {
        // No explicit Texture::RegisterLoader(): ResourceLoader::Initialize registers the image loader itself
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_texture_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets");

        const auto bmp = CheckerBmp();
        WriteFile("textures/checker.bmp", bmp);
        // Every image extension, in any case. The decoder goes by the content, so BMP data loads under each name.
        WriteFile("textures/Upper.BMP", bmp);
        WriteFile("textures/photo.JPG", bmp);
        WriteFile("textures/photo2.jpeg", bmp);
        WriteFile("textures/icon.Png", bmp);
        WriteFile("textures/decal.tga", bmp);

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "TextureAssetTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    // Writes settings into an asset's .meta and rescans, as editing it in the editor and reopening would
    static void SetCustomData(const std::string &relative, const json &customData)
    {
        IO::AssetMetadata meta = IO::AssetMetadata::FromFile(MetaPath(relative));
        meta.customData = customData;
        ASSERT_TRUE(meta.SaveToFile(MetaPath(relative)));
        IO::ResourceLoader::Instance().Initialize(s_root);
    }
};

TEST_F(TextureAssetTest, ImageFilesOfEveryExtensionAndCaseAreScannedAsTextures)
{
    auto &loader = IO::ResourceLoader::Instance();
    for (const std::string relative : {"textures/checker.bmp", "textures/Upper.BMP", "textures/photo.JPG",
                                       "textures/photo2.jpeg", "textures/icon.Png", "textures/decal.tga"})
    {
        const IO::ResourcePath path("res://" + relative);
        const IO::AssetMetadata *meta = loader.GetMetadata(path);
        ASSERT_NE(meta, nullptr) << relative;
        EXPECT_EQ(meta->resourceType, "Texture") << relative;
        EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(path)) << relative;
        EXPECT_TRUE(fs::exists(MetaPath(relative))) << relative;
    }
    EXPECT_EQ(loader.GetAssetsByType("Texture").size(), 6u);
}

TEST_F(TextureAssetTest, UuidsAreStableAcrossRescansAndRestarts)
{
    auto &loader = IO::ResourceLoader::Instance();
    const Math::UUID before = loader.GetUUID(kCheckerPath);
    ASSERT_NE(before, Math::UUID::ZERO);

    loader.RescanAssets();
    EXPECT_EQ(loader.GetUUID(kCheckerPath), before);
    loader.Initialize(s_root); // a restart: the .meta files are read back
    EXPECT_EQ(loader.GetUUID(kCheckerPath), before);
    EXPECT_EQ(IO::ResourceUUID::FromPath(kCheckerPath), before) << "derived from the path";
}

TEST_F(TextureAssetTest, LoadsWithTheDefaultSettings)
{
    const auto texture = IO::ResourceLoader::Instance().Load<Texture>(kCheckerPath);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->IsLoaded());
    EXPECT_EQ(texture->GetUUID(), IO::ResourceLoader::Instance().GetUUID(kCheckerPath));
    EXPECT_EQ(texture->GetResourcePath(), kCheckerPath);
    EXPECT_EQ(texture->GetSettings(), TextureSettings{});
    EXPECT_FALSE(texture->GetSourcePath().empty());
    EXPECT_EQ(PixelAt(*texture, 0, 0), (Rgba{0, 0, 255, 255})) << "bottom row first";
    EXPECT_EQ(PixelAt(*texture, 0, 1), (Rgba{255, 0, 0, 255}));

    // An upper-case extension loads too
    const auto upper = IO::ResourceLoader::Instance().Load<Texture>(IO::ResourcePath("res://textures/Upper.BMP"));
    ASSERT_NE(upper, nullptr);
    EXPECT_EQ(upper->GetPixels(), texture->GetPixels());
}

TEST_F(TextureAssetTest, LoadsWithTheSettingsFromItsMeta)
{
    SetCustomData("textures/checker.bmp", {{"texture", {{"srgb", false}, {"filter", "nearest"}, {"wrap", "clamp"},
                                                        {"mipmaps", false}, {"flipY", false}}}});

    const auto texture = IO::ResourceLoader::Instance().Load<Texture>(kCheckerPath);
    ASSERT_NE(texture, nullptr);
    const TextureSettings &settings = texture->GetSettings();
    EXPECT_FALSE(settings.srgb) << "recorded";
    EXPECT_EQ(settings.filter, TextureFilter::Nearest);
    EXPECT_EQ(settings.wrap, TextureWrap::ClampToEdge);
    EXPECT_FALSE(settings.mipmaps);
    EXPECT_FALSE(settings.flipY);
    EXPECT_EQ(PixelAt(*texture, 0, 0), (Rgba{255, 0, 0, 255})) << "flipY off: the top row first";
}

TEST_F(TextureAssetTest, BadSettingsInTheMetaWarnAndKeepTheDefaults)
{
    SetCustomData("textures/checker.bmp", {{"texture", {{"filter", "bicubic"}, {"wrap", "mirror"}}}});

    WarningCapture capture;
    const auto texture = IO::ResourceLoader::Instance().Load<Texture>(kCheckerPath);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetSettings(), TextureSettings{});
    const auto settingsWarnings = std::ranges::count_if(capture.warnings, [](const std::string &warning)
    {
        return warning.starts_with("Texture settings");
    });
    EXPECT_EQ(settingsWarnings, 2);
}

TEST_F(TextureAssetTest, LoadsThroughResourcesWithItsMetaUuid)
{
    const auto texture = IO::Resources::Instance().Load<Texture>("res://textures/checker.bmp");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetUUID(), IO::ResourceLoader::Instance().GetUUID(kCheckerPath));
    EXPECT_EQ(IO::ResourceLoader::Instance().GetCached<Texture>(kCheckerPath), texture);
    EXPECT_EQ(IO::Resources::Instance().LoadByUUID<Texture>(texture->GetUUID()), texture);
}

TEST_F(TextureAssetTest, ACorruptImageFailsToLoad)
{
    const std::string text = "not an image";
    WriteFile("textures/broken.png", std::vector<std::uint8_t>(text.begin(), text.end()));
    IO::ResourceLoader::Instance().RescanAssets();

    const IO::ResourcePath path("res://textures/broken.png");
    ASSERT_TRUE(IO::ResourceLoader::Instance().Exists(path));
    EXPECT_EQ(IO::ResourceLoader::Instance().GetMetadata(path)->resourceType, "Texture");
    EXPECT_EQ(IO::ResourceLoader::Instance().Load<Texture>(path), nullptr);
}

TEST_F(TextureAssetTest, AnImagesSpriteSurvivesASceneSaveAndLoad)
{
    const auto texture = IO::Resources::Instance().Load<Texture>("res://textures/checker.bmp");
    ASSERT_NE(texture, nullptr);

    const auto scene = Scene::Create("TextureAsset_Sprite");
    const auto go = GameObject::Create("Icon");
    auto *image = go->AddComponent<UI::Image>();
    image->SetSprite(texture);
    scene->AddRootGameObject(go);

    const json saved = scene->Serialize();
    EXPECT_NE(saved.dump().find(texture->GetUUID().ToString()), std::string::npos) << "saved as the asset's UUID";

    // Loaded while the texture is cached: the same asset
    {
        const auto loaded = Scene::FromJSON(saved);
        ASSERT_NE(loaded, nullptr);
        const auto loadedGo = loaded->FindGameObject("Icon");
        ASSERT_NE(loadedGo, nullptr);
        const auto *loadedImage = loadedGo->GetComponent<UI::Image>();
        ASSERT_NE(loadedImage, nullptr);
        EXPECT_EQ(loadedImage->GetSprite(), texture);
    }

    // Loaded after the cache is cleared (a new run): loaded again from the file by its UUID
    image->SetSprite(nullptr);
    const Math::UUID uuid = texture->GetUUID();
    IO::ResourceLoader::Instance().ClearCache();
    {
        const auto loaded = Scene::FromJSON(saved);
        ASSERT_NE(loaded, nullptr);
        const auto *loadedImage = loaded->FindGameObject("Icon")->GetComponent<UI::Image>();
        ASSERT_NE(loadedImage, nullptr);
        ASSERT_NE(loadedImage->GetSprite(), nullptr);
        EXPECT_EQ(loadedImage->GetSprite()->GetUUID(), uuid);
        EXPECT_EQ(loadedImage->GetSprite()->GetResourcePath(), kCheckerPath);
        EXPECT_EQ(loadedImage->GetSprite()->GetPixels(), texture->GetPixels());
    }
}

TEST(ImageSpriteSerializationTest, NoSpriteSavesNullAndLoadsAsNone)
{
    const auto scene = Scene::Create("ImageSprite_None");
    const auto go = GameObject::Create("Plain");
    go->AddComponent<UI::Image>();
    scene->AddRootGameObject(go);

    const auto loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto *image = loaded->FindGameObject("Plain")->GetComponent<UI::Image>();
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->GetSprite(), nullptr);
}

TEST(ImageSpriteSerializationTest, ARuntimeRegisteredSpriteRoundTripsInTheSameRun)
{
    const auto texture = Texture::Create(1, 1, std::vector<std::uint8_t>{10, 20, 30, 255});
    ASSERT_NE(texture, nullptr);
    IO::Resources::Instance().RegisterAsset(texture);

    const auto scene = Scene::Create("ImageSprite_Runtime");
    const auto go = GameObject::Create("Runtime");
    go->AddComponent<UI::Image>()->SetSprite(texture);
    scene->AddRootGameObject(go);

    const auto loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->FindGameObject("Runtime")->GetComponent<UI::Image>()->GetSprite(), texture);

    IO::Resources::Instance().UnregisterAsset(texture->GetUUID());
}
