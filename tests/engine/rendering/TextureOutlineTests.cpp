#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <renderer/common/TextureOptions.hpp>

#include "engine/Logger.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Texture.hpp"

// Texture::MakeOutline (docs/meshes.html#outline, effects design stage E3): the dilated alpha mask, its padding, width and
// colour, exact pixel by pixel on small sources; the edge cases; the settings it keeps; and its cache

using namespace N2Engine;
using Rendering::Texture;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;

namespace
{
    using Rgba = std::array<std::uint8_t, 4>;

    constexpr Rgba kClear{0, 0, 0, 0};
    constexpr Rgba kOpaqueRed{255, 0, 0, 255};
    constexpr Rgba kOpaqueBlue{0, 0, 255, 255};
    const Common::Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};
    constexpr Rgba kGreenBytes{0, 255, 0, 255};

    Rgba PixelAt(const Texture &texture, const std::uint32_t x, const std::uint32_t row)
    {
        const std::size_t i = (static_cast<std::size_t>(row) * texture.GetWidth() + x) * 4;
        const auto &p = texture.GetPixels();
        return Rgba{p[i], p[i + 1], p[i + 2], p[i + 3]};
    }

    /// A texture from one RGBA value per pixel, the first row being v = 0
    std::shared_ptr<Texture> MakeSource(const std::uint32_t width, const std::uint32_t height, const std::vector<Rgba> &texels,
                                        const TextureOptions &options = TextureOptions::Default())
    {
        std::vector<std::uint8_t> bytes;
        for (const Rgba &texel : texels)
        {
            bytes.insert(bytes.end(), texel.begin(), texel.end());
        }
        return Texture::Create(width, height, bytes, options);
    }

    /// `texture` as rows of '#' (the outline colour `expected`) and '.' (transparent black), first row first. Any other
    /// value is '?', so a wrong colour shows up in the comparison
    std::vector<std::string> Picture(const Texture &texture, const Rgba &expected)
    {
        std::vector<std::string> rows;
        for (std::uint32_t row = 0; row < texture.GetHeight(); ++row)
        {
            std::string line;
            for (std::uint32_t x = 0; x < texture.GetWidth(); ++x)
            {
                const Rgba pixel = PixelAt(texture, x, row);
                line += pixel == expected ? '#' : pixel == kClear ? '.' : '?';
            }
            rows.push_back(line);
        }
        return rows;
    }

    /// The outline by its definition, checking every source texel for every pixel: solid where a texel with alpha >= 128 is
    /// within `width` (Euclidean) of the pixel's place in the source
    std::vector<std::string> BruteForce(const std::uint32_t width, const std::uint32_t height, const std::vector<Rgba> &texels,
                                        const int outline)
    {
        const int padded = 2 * outline;
        std::vector<std::string> rows;
        for (int row = 0; row < static_cast<int>(height) + padded; ++row)
        {
            std::string line;
            for (int x = 0; x < static_cast<int>(width) + padded; ++x)
            {
                bool solid = false;
                for (int sy = 0; sy < static_cast<int>(height) && !solid; ++sy)
                {
                    for (int sx = 0; sx < static_cast<int>(width) && !solid; ++sx)
                    {
                        const int dx = x - outline - sx;
                        const int dy = row - outline - sy;
                        solid = texels[static_cast<std::size_t>(sy) * width + static_cast<std::size_t>(sx)][3] >= 128 &&
                                dx * dx + dy * dy <= outline * outline;
                    }
                }
                line += solid ? '#' : '.';
            }
            rows.push_back(line);
        }
        return rows;
    }

    /// Collects the errors logged while it lives
    class ErrorCapture
    {
    public:
        ErrorCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Error)
                {
                    errors.emplace_back(message);
                }
            };
        }
        ~ErrorCapture() { Logger::logEvent -= _id; }
        ErrorCapture(const ErrorCapture &) = delete;
        ErrorCapture &operator=(const ErrorCapture &) = delete;

        std::vector<std::string> errors;

    private:
        size_t _id = 0;
    };

    class TextureOutlineTest : public ::testing::Test
    {
    protected:
        void SetUp() override { Texture::ClearOutlineCache(); }
        void TearDown() override { Texture::ClearOutlineCache(); }
    };
}

// ===== The pixels =====

TEST_F(TextureOutlineTest, OneOpaqueTexelGrowsIntoADiscPaddedByTheWidthOnEverySide)
{
    const auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);

    const auto outline = Texture::MakeOutline(source, 2, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetWidth(), 5u) << "1 + 2 * 2";
    EXPECT_EQ(outline->GetHeight(), 5u);
    // Within 2 of the middle: dx^2 + dy^2 <= 4
    const std::vector<std::string> expected = {
        "..#..",
        ".###.",
        "#####",
        ".###.",
        "..#..",
    };
    EXPECT_EQ(Picture(*outline, kGreenBytes), expected);
}

TEST_F(TextureOutlineTest, TheSourceSitsInTheMiddleWithTheSameRowOrder)
{
    // 3 x 2, rows first to last: only the first texel of the first row is solid
    const auto source = MakeSource(3, 2, {kOpaqueRed, kClear, kClear, kClear, kClear, kClear});
    ASSERT_NE(source, nullptr);

    const auto outline = Texture::MakeOutline(source, 1, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetWidth(), 5u);
    EXPECT_EQ(outline->GetHeight(), 4u);
    const std::vector<std::string> expected = {
        ".#...",
        "###..",
        ".#...",
        ".....",
    };
    EXPECT_EQ(Picture(*outline, kGreenBytes), expected);
}

TEST_F(TextureOutlineTest, WidthZeroIsTheSilhouetteAtTheSameSize)
{
    const auto source = MakeSource(3, 1, {kClear, kOpaqueRed, kOpaqueBlue});
    ASSERT_NE(source, nullptr);

    const auto outline = Texture::MakeOutline(source, 0, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetWidth(), 3u);
    EXPECT_EQ(outline->GetHeight(), 1u);
    EXPECT_EQ(Picture(*outline, kGreenBytes), std::vector<std::string>{".##"}) << "every solid texel in the one colour";
}

TEST_F(TextureOutlineTest, ATexelIsSolidFromHalfOpaque)
{
    const auto source = MakeSource(2, 1, {Rgba{255, 255, 255, 127}, Rgba{255, 255, 255, 128}});
    ASSERT_NE(source, nullptr);
    static_assert(Texture::OutlineSolidAlpha == 128);

    const auto outline = Texture::MakeOutline(source, 0, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(Picture(*outline, kGreenBytes), std::vector<std::string>{".#"}) << "127 is under half, 128 is not";
}

TEST_F(TextureOutlineTest, MatchesTheDefinitionOnALargerShapeAtSeveralWidths)
{
    // A ring, a diagonal and a lone texel, so the distance transform's envelope is exercised across rows and columns
    constexpr std::uint32_t width = 13;
    constexpr std::uint32_t height = 9;
    std::vector<Rgba> texels(width * height, kClear);
    const auto set = [&](const std::uint32_t x, const std::uint32_t y) { texels[y * width + x] = kOpaqueRed; };
    for (std::uint32_t i = 1; i <= 4; ++i)
    {
        set(i, 1);
        set(i, 4);
        set(1, i);
        set(4, i);
    }
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        set(6 + i, 2 + i);
    }
    set(12, 0);
    texels[8 * width + 2] = Rgba{9, 9, 9, 200}; // partly transparent but solid

    const auto source = MakeSource(width, height, texels);
    ASSERT_NE(source, nullptr);
    for (const int outlineWidth : {0, 1, 2, 3, 5, 8})
    {
        const auto outline = Texture::MakeOutline(source, outlineWidth, kGreen);
        ASSERT_NE(outline, nullptr) << outlineWidth;
        EXPECT_EQ(Picture(*outline, kGreenBytes), BruteForce(width, height, texels, outlineWidth)) << "width " << outlineWidth;
    }
}

TEST_F(TextureOutlineTest, AFullyTransparentSourceGivesAFullyTransparentPaddedTexture)
{
    const auto source = MakeSource(3, 2, std::vector<Rgba>(6, Rgba{255, 255, 255, 0}));
    ASSERT_NE(source, nullptr);

    const auto outline = Texture::MakeOutline(source, 2, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetWidth(), 7u);
    EXPECT_EQ(outline->GetHeight(), 6u);
    for (const std::uint8_t byte : outline->GetPixels())
    {
        ASSERT_EQ(byte, 0);
    }
}

TEST_F(TextureOutlineTest, TheColourIsClampedAndStoredAsEightBitsWithItsAlpha)
{
    const auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);
    const Common::Color colour{2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f};

    const auto outline = Texture::MakeOutline(source, 0, colour);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(PixelAt(*outline, 0, 0), (Rgba{255, 0, 0, 128})) << "over 1 is 1, under 0 and NaN are 0, 0.5 rounds to 128";
}

TEST_F(TextureOutlineTest, KeepsTheSourcesSettingsButClampsTheWrap)
{
    TextureOptions options;
    options.filter = TextureFilter::Nearest;
    options.wrap = TextureWrap::Repeat;
    options.mipmaps = false;
    const auto source = MakeSource(1, 1, {kOpaqueRed}, options);
    ASSERT_NE(source, nullptr);

    const auto outline = Texture::MakeOutline(source, 1, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(outline->GetSettings().filter, TextureFilter::Nearest);
    EXPECT_FALSE(outline->GetSettings().mipmaps);
    EXPECT_EQ(outline->GetSettings().srgb, source->GetSettings().srgb);
    EXPECT_EQ(outline->GetSettings().wrap, TextureWrap::ClampToEdge);
    EXPECT_TRUE(outline->GetSourcePath().empty()) << "a runtime texture";
    EXPECT_NE(outline->GetUUID(), source->GetUUID());
}

// ===== What it refuses =====

TEST_F(TextureOutlineTest, RefusesWhatItCannotOutlineWithAnError)
{
    const auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);
    ErrorCapture capture;

    EXPECT_EQ(Texture::MakeOutline(nullptr, 1, kGreen), nullptr);
    EXPECT_EQ(Texture::MakeOutline(std::make_shared<Texture>(), 1, kGreen), nullptr) << "no pixels";
    EXPECT_EQ(Texture::MakeOutline(source, -1, kGreen), nullptr);
    EXPECT_EQ(Texture::MakeOutline(source, Texture::MaxOutlineWidth + 1, kGreen), nullptr);
    EXPECT_EQ(Texture::MakeOutline(source, std::numeric_limits<int>::max(), kGreen), nullptr);
    EXPECT_EQ(capture.errors.size(), 5u);
    EXPECT_NE(Texture::MakeOutline(source, Texture::MaxOutlineWidth, kGreen), nullptr) << "the widest allowed";

    // 16384 wide is the cap for a texture, so the padding takes it over
    const auto wide = Texture::Create(16384, 1, std::vector<std::uint8_t>(16384u * 4u, 255));
    ASSERT_NE(wide, nullptr);
    capture.errors.clear();
    EXPECT_EQ(Texture::MakeOutline(wide, 1, kGreen), nullptr);
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_NE(capture.errors[0].find("size caps"), std::string::npos) << capture.errors[0];
    EXPECT_NE(Texture::MakeOutline(wide, 0, kGreen), nullptr) << "its silhouette still fits";
}

// ===== The cache =====

TEST_F(TextureOutlineTest, TheSameSourceWidthAndColourGiveTheSameTexture)
{
    const auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);

    const auto first = Texture::MakeOutline(source, 2, kGreen);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(Texture::MakeOutline(source, 2, kGreen), first) << "a cache hit, not a rebuild";
    EXPECT_EQ(Texture::MakeOutline(source, 2, Common::Color{0.0f, 1.0001f, 0.001f, 1.0f}), first)
        << "colours that store the same bytes share an outline";
    EXPECT_EQ(Texture::GetOutlineCacheSize(), 1u);

    EXPECT_NE(Texture::MakeOutline(source, 3, kGreen), first) << "another width";
    EXPECT_NE(Texture::MakeOutline(source, 2, Common::Color{0.0f, 1.0f, 0.0f, 0.5f}), first) << "another alpha";
    const auto twin = MakeSource(1, 1, {kOpaqueRed});
    EXPECT_NE(Texture::MakeOutline(twin, 2, kGreen), first) << "another source object, even with the same pixels";
    EXPECT_EQ(Texture::GetOutlineCacheSize(), 4u);
}

TEST_F(TextureOutlineTest, AFreedSourcesOutlinesLeaveTheCache)
{
    auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);
    const auto outline = Texture::MakeOutline(source, 1, kGreen);
    ASSERT_NE(outline, nullptr);
    EXPECT_EQ(Texture::GetOutlineCacheSize(), 1u);

    source.reset();
    EXPECT_EQ(Texture::GetOutlineCacheSize(), 0u) << "the cache held the source only weakly";
    EXPECT_EQ(outline->GetWidth(), 3u) << "an outline in use stays alive with its user";
}

TEST_F(TextureOutlineTest, TheLeastRecentlyUsedOutlineIsDroppedAtCapacity)
{
    const auto source = MakeSource(1, 1, {kOpaqueRed});
    ASSERT_NE(source, nullptr);

    const auto oldest = Texture::MakeOutline(source, 0, kGreen);
    const auto kept = Texture::MakeOutline(source, 1, kGreen);
    for (int width = 2; width < static_cast<int>(Texture::OutlineCacheCapacity); ++width)
    {
        ASSERT_NE(Texture::MakeOutline(source, width, kGreen), nullptr);
    }
    EXPECT_EQ(Texture::GetOutlineCacheSize(), Texture::OutlineCacheCapacity);
    EXPECT_EQ(Texture::MakeOutline(source, 1, kGreen), kept) << "asked for again, so now the most recent";

    ASSERT_NE(Texture::MakeOutline(source, static_cast<int>(Texture::OutlineCacheCapacity), kGreen), nullptr);
    EXPECT_EQ(Texture::GetOutlineCacheSize(), Texture::OutlineCacheCapacity) << "one in, one out";
    EXPECT_EQ(Texture::MakeOutline(source, 1, kGreen), kept) << "still cached";
    EXPECT_NE(Texture::MakeOutline(source, 0, kGreen), oldest) << "width 0 was the least recently used: made again";
}
