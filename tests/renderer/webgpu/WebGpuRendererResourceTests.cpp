#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuTestFixture.hpp"

// The WebGPU backend's resources (#155, stage G3a): meshes, textures, samplers, CPU-generated mipmaps, UpdateMesh with its
// in-frame rule, lifetimes and device loss. Written to mirror the Vulkan backend's stage A tests
// (tests/renderer/vulkan/VulkanResourceTests.cpp) where the behaviour is shared and to say where it is not (there is no
// deferred deletion: WebGPU keeps an object alive for every command that uses it, so the tests prove that destroying inside a
// frame is safe, and that the live object count returns to its baseline). The tests prove what was uploaded through the test
// hooks (ReadTextureForTest, ReadMeshForTest, GetTextureInfoForTest, GetMeshInfoForTest): they copy it back through a buffer
// with rows padded to 256 bytes, which the texture sizes below (37 x 5 texels, 5 x 3, 70 x 2) exercise on purpose.
//
// Every test runs under the fixture's rules: the device reported no error and no WebGPU object outlived the renderers.

using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::Vertex;
using Renderer::WebGpu::MeshInfoForTest;
using Renderer::WebGpu::TextureInfoForTest;
using Renderer::WebGpu::WebGpuRenderer;
using namespace WebGpuTesting;

namespace
{
    class WebGpuResourceTest : public WebGpuTest
    {
    protected:
        /// A second initialized renderer, whose resources are foreign to R(); skips like SetUp when there is no device
        WebGpuRenderer* NewInitializedRenderer()
        {
            WebGpuRenderer& renderer = NewRenderer();
            if (!renderer.Initialize(nullptr, 8, 8))
            {
                HandleInitFailure(renderer);
                return nullptr;
            }
            return &renderer;
        }
    };

    /// A vertex whose every float is distinct (a function of `seed` and the slot), so a copy that drops, reorders or shifts
    /// anything shows
    Vertex MakeVertex(int seed)
    {
        Vertex v{};
        float* f = reinterpret_cast<float*>(&v);
        for (std::size_t i = 0; i < sizeof(Vertex) / sizeof(float); ++i)
        {
            f[i] = static_cast<float>(seed) * 0.25f + static_cast<float>(i) * 0.015625f + 1.0f;
        }
        return v;
    }

    MeshData MakeMeshData(std::size_t vertexCount, std::size_t indexCount, int seed)
    {
        MeshData data;
        for (std::size_t i = 0; i < vertexCount; ++i)
        {
            data.vertices.push_back(MakeVertex(seed * 1000 + static_cast<int>(i)));
        }
        for (std::size_t i = 0; i < indexCount; ++i)
        {
            data.indices.push_back(static_cast<std::uint32_t>((i * 7 + static_cast<std::size_t>(seed)) % vertexCount));
        }
        return data;
    }

    ::testing::AssertionResult SameMesh(const MeshData& expected, const MeshData& actual)
    {
        if (expected.vertices.size() != actual.vertices.size() ||
            (!expected.vertices.empty() &&
             std::memcmp(expected.vertices.data(), actual.vertices.data(), expected.vertices.size() * sizeof(Vertex)) != 0))
        {
            return ::testing::AssertionFailure() << "the vertices differ (" << expected.vertices.size() << " expected, "
                                                  << actual.vertices.size() << " read)";
        }
        if (expected.indices != actual.indices)
        {
            return ::testing::AssertionFailure() << "the indices differ (" << expected.indices.size() << " expected, "
                                                  << actual.indices.size() << " read)";
        }
        return ::testing::AssertionSuccess();
    }

    /// A quad over the whole frame (NDC, identity matrices) with white vertices
    MeshData FullQuad()
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// width x height x channels bytes, tightly packed, every byte a function of (x, y, channel, seed)
    std::vector<std::uint8_t> Pattern(std::uint32_t width, std::uint32_t height, std::uint32_t channels, int seed = 0)
    {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * channels);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                for (std::uint32_t c = 0; c < channels; ++c)
                {
                    bytes[(static_cast<std::size_t>(y) * width + x) * channels + c] =
                        static_cast<std::uint8_t>((x * 31 + y * 17 + c * 59 + static_cast<std::uint32_t>(seed) * 7 + 3) % 256);
                }
            }
        }
        return bytes;
    }

    /// Level 0 as the renderer stores it: 3 channels expanded to RGBA with alpha 255, the others as given
    std::vector<std::uint8_t> StoredLevel0(const std::vector<std::uint8_t>& source, std::uint32_t texels,
                                           std::uint32_t channels)
    {
        if (channels != 3)
        {
            return source;
        }
        std::vector<std::uint8_t> expanded(static_cast<std::size_t>(texels) * 4);
        for (std::size_t i = 0; i < texels; ++i)
        {
            expanded[i * 4 + 0] = source[i * 3 + 0];
            expanded[i * 4 + 1] = source[i * 3 + 1];
            expanded[i * 4 + 2] = source[i * 3 + 2];
            expanded[i * 4 + 3] = 255;
        }
        return expanded;
    }

    WGPUTextureFormat ExpectedFormat(std::uint32_t channels, bool srgb)
    {
        if (channels == 1)
        {
            return WGPUTextureFormat_R8Unorm;
        }
        if (channels == 2)
        {
            return WGPUTextureFormat_RG8Unorm;
        }
        return srgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
    }

    std::uint32_t StoredChannels(std::uint32_t channels)
    {
        return channels == 3 ? 4 : channels;
    }

    TextureOptions Options(TextureFilter filter, TextureWrap wrap, bool mipmaps, bool srgb = false)
    {
        TextureOptions options;
        options.filter = filter;
        options.wrap = wrap;
        options.mipmaps = mipmaps;
        options.srgb = srgb;
        return options;
    }

    /// The next mip level the way the renderer builds it (see WebGpuResources.cpp, Downsample): each destination texel is the
    /// equal-weight average of the source texels it covers, rounded half up. For the usual halving that is the 2x2 box
    std::vector<std::uint8_t> ReferenceDownsample(const std::vector<std::uint8_t>& source, std::uint32_t sourceWidth,
                                                  std::uint32_t sourceHeight, std::uint32_t destinationWidth,
                                                  std::uint32_t destinationHeight, std::uint32_t texelBytes)
    {
        std::vector<std::uint8_t> destination(static_cast<std::size_t>(destinationWidth) * destinationHeight * texelBytes);
        for (std::uint32_t y = 0; y < destinationHeight; ++y)
        {
            const std::uint32_t y0 = y * sourceHeight / destinationHeight;
            const std::uint32_t y1 = std::min(((y + 1) * sourceHeight + destinationHeight - 1) / destinationHeight, sourceHeight);
            for (std::uint32_t x = 0; x < destinationWidth; ++x)
            {
                const std::uint32_t x0 = x * sourceWidth / destinationWidth;
                const std::uint32_t x1 = std::min(((x + 1) * sourceWidth + destinationWidth - 1) / destinationWidth, sourceWidth);
                const std::uint32_t count = (x1 - x0) * (y1 - y0);
                for (std::uint32_t c = 0; c < texelBytes; ++c)
                {
                    std::uint32_t sum = 0;
                    for (std::uint32_t sy = y0; sy < y1; ++sy)
                    {
                        for (std::uint32_t sx = x0; sx < x1; ++sx)
                        {
                            sum += source[(static_cast<std::size_t>(sy) * sourceWidth + sx) * texelBytes + c];
                        }
                    }
                    destination[(static_cast<std::size_t>(y) * destinationWidth + x) * texelBytes + c] =
                        static_cast<std::uint8_t>((sum + count / 2) / count);
                }
            }
        }
        return destination;
    }
}

// ============================================================================
// Textures: creation, formats, level 0
// ============================================================================

TEST_F(WebGpuResourceTest, TextureCreationRejectsNullDataZeroSizeAndBadChannelCounts)
{
    const std::uint8_t texel[16] = {};
    EXPECT_EQ(R().CreateTexture(nullptr, 1, 1, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 0, 1, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1, 0, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1, 1, 0), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1, 1, 5), nullptr);
    EXPECT_EQ(R().CreateTexture(nullptr, 1, 1, 4, TextureOptions{}), nullptr);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetValidationErrorCount(), 0u) << "a rejected call is not a WebGPU error";
}

TEST_F(WebGpuResourceTest, TextureCreationRejectsASizeOverTheDeviceLimitWithoutAWebGpuError)
{
    const std::uint8_t texel[4] = {1, 2, 3, 4}; // never read: the size is checked first
    const std::uint32_t limit = R().GetMaxTextureDimensionForTest();
    ASSERT_GT(limit, 0u);
    EXPECT_EQ(R().CreateTexture(texel, limit + 1, 1, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1, limit + 1, 4), nullptr);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuResourceTest, EveryChannelCountAndOptionCreatesATextureWhoseLevelZeroHoldsTheExactBytes)
{
    // 37 x 5: a row of 148 bytes (RGBA) is padded to 256 in the readback copy, 37 bytes (R8) to 256: the padding is stripped
    // and what is left is exactly what was given
    constexpr std::uint32_t Width = 37;
    constexpr std::uint32_t Height = 5;
    int seed = 0;
    for (std::uint32_t channels = 1; channels <= 4; ++channels)
    {
        for (const bool srgb : {false, true})
        {
            for (const bool mipmaps : {false, true})
            {
                const std::vector<std::uint8_t> source = Pattern(Width, Height, channels, ++seed);
                const TextureOptions options = Options(TextureFilter::Linear, TextureWrap::Repeat, mipmaps, srgb);
                ITexture* texture = R().CreateTexture(source.data(), Width, Height, channels, options);
                ASSERT_NE(texture, nullptr) << channels << " channels, srgb " << srgb << ", mipmaps " << mipmaps;
                EXPECT_TRUE(texture->IsValid());
                EXPECT_EQ(texture->GetWidth(), Width);
                EXPECT_EQ(texture->GetHeight(), Height);
                EXPECT_EQ(texture->GetChannels(), channels);

                const TextureInfoForTest info = R().GetTextureInfoForTest(texture);
                ASSERT_TRUE(info.valid);
                EXPECT_EQ(info.format, ExpectedFormat(channels, srgb)) << channels << " channels, srgb " << srgb;
                EXPECT_EQ(info.width, Width);
                EXPECT_EQ(info.height, Height);
                EXPECT_EQ(info.channels, channels);
                EXPECT_EQ(info.mipLevels, mipmaps ? 6u : 1u) << "37 x 5 has 6 levels (37, 18, 9, 4, 2, 1)";

                std::vector<std::uint8_t> bytes;
                std::uint32_t width = 0;
                std::uint32_t height = 0;
                ASSERT_TRUE(R().ReadTextureForTest(texture, 0, bytes, &width, &height));
                EXPECT_EQ(width, Width);
                EXPECT_EQ(height, Height);
                EXPECT_EQ(bytes, StoredLevel0(source, Width * Height, channels))
                    << channels << " channels, srgb " << srgb << ", mipmaps " << mipmaps;
                EXPECT_EQ(bytes.size(), static_cast<std::size_t>(Width) * Height * StoredChannels(channels));
                R().DestroyTexture(texture);
            }
        }
    }
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
}

TEST_F(WebGpuResourceTest, ARowWiderThanTheCopyAlignmentIsReadBackWithoutItsPadding)
{
    // 70 x 2 RGBA: 280 bytes per row, padded to 512 in the copy; 3 channels of 70 x 2 too (expanded to the same 280)
    for (const std::uint32_t channels : {3u, 4u})
    {
        const std::vector<std::uint8_t> source = Pattern(70, 2, channels, 9);
        ITexture* texture = R().CreateTexture(source.data(), 70, 2, channels, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, false));
        ASSERT_NE(texture, nullptr);
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
        EXPECT_EQ(bytes, StoredLevel0(source, 70 * 2, channels)) << channels << " channels";
    }
}

TEST_F(WebGpuResourceTest, TheOverloadWithoutOptionsUsesTheDefaults)
{
    const std::vector<std::uint8_t> source = Pattern(8, 8, 4);
    ITexture* plain = R().CreateTexture(source.data(), 8, 8, 4);
    ITexture* defaults = R().CreateTexture(source.data(), 8, 8, 4, TextureOptions::Default());
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(defaults, nullptr);
    const TextureInfoForTest a = R().GetTextureInfoForTest(plain);
    const TextureInfoForTest b = R().GetTextureInfoForTest(defaults);
    EXPECT_EQ(a.format, b.format);
    EXPECT_EQ(a.mipLevels, b.mipLevels);
    EXPECT_EQ(a.mipLevels, 4u) << "mipmaps are on by default: 8, 4, 2, 1";
    EXPECT_EQ(a.filter, WGPUFilterMode_Linear);
    EXPECT_EQ(a.mipmapFilter, WGPUMipmapFilterMode_Linear);
    EXPECT_EQ(a.addressMode, WGPUAddressMode_Repeat);
    EXPECT_EQ(a.sampler, b.sampler) << "equal options share one sampler";
}

TEST_F(WebGpuResourceTest, SamplersAreCachedByOptionsAndDifferentOptionsGetDifferentSamplers)
{
    const std::vector<std::uint8_t> source = Pattern(4, 4, 4);
    struct Case
    {
        TextureFilter filter;
        TextureWrap wrap;
        WGPUFilterMode expectedFilter;
        WGPUMipmapFilterMode expectedMipmapFilter;
        WGPUAddressMode expectedAddress;
    };
    const Case cases[] = {
        {TextureFilter::Nearest, TextureWrap::Repeat, WGPUFilterMode_Nearest, WGPUMipmapFilterMode_Nearest, WGPUAddressMode_Repeat},
        {TextureFilter::Nearest, TextureWrap::ClampToEdge, WGPUFilterMode_Nearest, WGPUMipmapFilterMode_Nearest, WGPUAddressMode_ClampToEdge},
        {TextureFilter::Linear, TextureWrap::Repeat, WGPUFilterMode_Linear, WGPUMipmapFilterMode_Linear, WGPUAddressMode_Repeat},
        {TextureFilter::Linear, TextureWrap::ClampToEdge, WGPUFilterMode_Linear, WGPUMipmapFilterMode_Linear, WGPUAddressMode_ClampToEdge},
    };
    std::vector<WGPUSampler> samplers;
    for (const Case& test : cases)
    {
        ITexture* first = R().CreateTexture(source.data(), 4, 4, 4, Options(test.filter, test.wrap, true));
        ITexture* second = R().CreateTexture(source.data(), 4, 4, 4, Options(test.filter, test.wrap, false));
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        const TextureInfoForTest a = R().GetTextureInfoForTest(first);
        const TextureInfoForTest b = R().GetTextureInfoForTest(second);
        EXPECT_EQ(a.filter, test.expectedFilter);
        EXPECT_EQ(a.mipmapFilter, test.expectedMipmapFilter);
        EXPECT_EQ(a.addressMode, test.expectedAddress);
        EXPECT_NE(a.sampler, nullptr);
        EXPECT_EQ(a.sampler, b.sampler) << "the mip chain is not part of the sampler";
        samplers.push_back(a.sampler);
    }
    for (std::size_t i = 0; i < samplers.size(); ++i)
    {
        for (std::size_t j = i + 1; j < samplers.size(); ++j)
        {
            EXPECT_NE(samplers[i], samplers[j]) << "cases " << i << " and " << j;
        }
    }
    // The sRGB flag and the channel count are not part of the sampler either
    ITexture* srgb = R().CreateTexture(source.data(), 4, 4, 4, Options(TextureFilter::Nearest, TextureWrap::Repeat, true, true));
    ASSERT_NE(srgb, nullptr);
    EXPECT_EQ(R().GetTextureInfoForTest(srgb).sampler, samplers[0]);
}

// ============================================================================
// Textures: mipmaps
// ============================================================================

TEST_F(WebGpuResourceTest, MipmapsOffGivesExactlyOneLevelAndLevelsBeyondItAreNotReadable)
{
    const std::vector<std::uint8_t> source = Pattern(8, 8, 4);
    ITexture* texture = R().CreateTexture(source.data(), 8, 8, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, false));
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(R().GetTextureInfoForTest(texture).mipLevels, 1u);
    std::vector<std::uint8_t> bytes;
    EXPECT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
    EXPECT_FALSE(R().ReadTextureForTest(texture, 1, bytes));
    EXPECT_FALSE(R().ReadTextureForTest(texture, 99, bytes));
    // With mipmaps on, the first level that does not exist is mipLevels itself
    ITexture* mipped = R().CreateTexture(source.data(), 8, 8, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true));
    ASSERT_NE(mipped, nullptr);
    EXPECT_TRUE(R().ReadTextureForTest(mipped, 3, bytes));
    EXPECT_FALSE(R().ReadTextureForTest(mipped, 4, bytes));
}

TEST_F(WebGpuResourceTest, MipLevelsHaveTheExpectedSizesForSquareNonSquareAndOddTextures)
{
    struct Case
    {
        std::uint32_t width, height;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> levels;
    };
    const Case cases[] = {
        {64, 64, {{64, 64}, {32, 32}, {16, 16}, {8, 8}, {4, 4}, {2, 2}, {1, 1}}},
        {5, 3, {{5, 3}, {2, 1}, {1, 1}}},
        {1, 1, {{1, 1}}},
        {8, 1, {{8, 1}, {4, 1}, {2, 1}, {1, 1}}},
        {1, 8, {{1, 8}, {1, 4}, {1, 2}, {1, 1}}},
        {7, 13, {{7, 13}, {3, 6}, {1, 3}, {1, 1}}},
        {37, 5, {{37, 5}, {18, 2}, {9, 1}, {4, 1}, {2, 1}, {1, 1}}},
    };
    for (const Case& test : cases)
    {
        const std::vector<std::uint8_t> source = Pattern(test.width, test.height, 4);
        ITexture* texture = R().CreateTexture(source.data(), test.width, test.height, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true));
        ASSERT_NE(texture, nullptr) << test.width << "x" << test.height;
        EXPECT_EQ(R().GetTextureInfoForTest(texture).mipLevels, test.levels.size()) << test.width << "x" << test.height;
        for (std::size_t level = 0; level < test.levels.size(); ++level)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            ASSERT_TRUE(R().ReadTextureForTest(texture, static_cast<std::uint32_t>(level), bytes, &width, &height))
                << test.width << "x" << test.height << " level " << level;
            EXPECT_EQ(width, test.levels[level].first) << test.width << "x" << test.height << " level " << level;
            EXPECT_EQ(height, test.levels[level].second) << test.width << "x" << test.height << " level " << level;
            EXPECT_EQ(bytes.size(), static_cast<std::size_t>(width) * height * 4);
        }
        std::vector<std::uint8_t> beyond;
        EXPECT_FALSE(R().ReadTextureForTest(texture, static_cast<std::uint32_t>(test.levels.size()), beyond));
    }
}

TEST_F(WebGpuResourceTest, AMipLevelOfAnExactTwoToOneReductionIsTheAverageOfEachTwoByTwoBlock)
{
    // 4 x 4 RGBA with a different value in every byte: level 1 (2 x 2) is each 2x2 block's per-channel average rounded half
    // up, level 2 (1 x 1) the average of those four, so a wrong block, a wrong rounding or a wrong order shows
    const std::vector<std::uint8_t> source = Pattern(4, 4, 4, 5);
    ITexture* texture = R().CreateTexture(source.data(), 4, 4, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true));
    ASSERT_NE(texture, nullptr);

    std::vector<std::uint8_t> expected1(2 * 2 * 4);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            for (int c = 0; c < 4; ++c)
            {
                const auto at = [&](int sx, int sy) { return static_cast<int>(source[(static_cast<std::size_t>(sy) * 4 + sx) * 4 + c]); };
                const int sum = at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1);
                expected1[(static_cast<std::size_t>(y) * 2 + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    std::vector<std::uint8_t> level1;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 1, level1));
    EXPECT_EQ(level1, expected1);

    std::vector<std::uint8_t> expected2(4);
    for (int c = 0; c < 4; ++c)
    {
        const int sum = expected1[0 + c] + expected1[4 + c] + expected1[8 + c] + expected1[12 + c];
        expected2[c] = static_cast<std::uint8_t>((sum + 2) / 4);
    }
    std::vector<std::uint8_t> level2;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 2, level2));
    EXPECT_EQ(level2, expected2);
}

TEST_F(WebGpuResourceTest, EveryMipLevelOfEveryChannelCountMatchesTheReferenceChain)
{
    // 5 x 3 (odd sizes: the covered ranges overlap by a texel) for 1, 2, 3 and 4 channels: each level is the reference
    // reduction of the one before it, the 3-channel texture as its RGBA expansion
    for (std::uint32_t channels = 1; channels <= 4; ++channels)
    {
        const std::vector<std::uint8_t> source = Pattern(5, 3, channels, 2);
        ITexture* texture = R().CreateTexture(source.data(), 5, 3, channels, Options(TextureFilter::Linear, TextureWrap::Repeat, true));
        ASSERT_NE(texture, nullptr);
        const std::uint32_t texelBytes = StoredChannels(channels);
        std::vector<std::uint8_t> expected = StoredLevel0(source, 5 * 3, channels);
        std::uint32_t width = 5;
        std::uint32_t height = 3;
        for (std::uint32_t level = 0; level < 3; ++level)
        {
            if (level > 0)
            {
                const std::uint32_t nextWidth = std::max(1u, width / 2);
                const std::uint32_t nextHeight = std::max(1u, height / 2);
                expected = ReferenceDownsample(expected, width, height, nextWidth, nextHeight, texelBytes);
                width = nextWidth;
                height = nextHeight;
            }
            std::vector<std::uint8_t> bytes;
            ASSERT_TRUE(R().ReadTextureForTest(texture, level, bytes));
            EXPECT_EQ(bytes, expected) << channels << " channels, level " << level;
        }
    }
}

TEST_F(WebGpuResourceTest, SrgbMipLevelsAreAveragedInLinearLightAndTheAlphaAsItIs)
{
    // Two black and two white texels: the naive byte average is 128, the average in linear light is 0.5 which encodes as 188
    // (0.5 ^ (1/2.4) * 1.055 - 0.055 = 0.7354). The alpha channel is averaged as stored. The data is read back raw (a copy
    // does not decode), so what is compared is what the chain stores
    const std::uint8_t texels[16] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0};
    ITexture* srgb = R().CreateTexture(texels, 2, 2, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true, true));
    ITexture* unorm = R().CreateTexture(texels, 2, 2, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true, false));
    ASSERT_NE(srgb, nullptr);
    ASSERT_NE(unorm, nullptr);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(R().ReadTextureForTest(srgb, 1, bytes));
    ASSERT_EQ(bytes.size(), 4u);
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_NEAR(static_cast<int>(bytes[c]), 188, 1) << "channel " << c << " in linear light";
    }
    EXPECT_EQ(bytes[3], 128) << "alpha (255 + 255 + 0 + 0) / 4 = 127.5, rounded half up";
    ASSERT_TRUE(R().ReadTextureForTest(unorm, 1, bytes));
    ASSERT_EQ(bytes.size(), 4u);
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_EQ(bytes[c], 128) << "a UNORM texture averages the bytes as they are";
    }
}

TEST_F(WebGpuResourceTest, ALargeTextureGetsTheWholeMipChain)
{
    const std::uint32_t width = 1024;
    const std::uint32_t height = 512;
    const std::vector<std::uint8_t> source = Pattern(width, height, 4, 1);
    ITexture* texture = R().CreateTexture(source.data(), width, height, 4, Options(TextureFilter::Linear, TextureWrap::Repeat, true));
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(R().GetTextureInfoForTest(texture).mipLevels, 11u) << "1024 x 512: 11 levels down to 1 x 1";
    std::vector<std::uint8_t> bytes;
    std::uint32_t levelWidth = 0;
    std::uint32_t levelHeight = 0;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 10, bytes, &levelWidth, &levelHeight));
    EXPECT_EQ(levelWidth, 1u);
    EXPECT_EQ(levelHeight, 1u);
    ASSERT_TRUE(R().ReadTextureForTest(texture, 9, bytes, &levelWidth, &levelHeight));
    EXPECT_EQ(levelWidth, 2u);
    EXPECT_EQ(levelHeight, 1u);
}

// ============================================================================
// Textures as the unlit shader samples them
// ============================================================================

TEST_F(WebGpuResourceTest, OneAndTwoChannelTexturesSampleAsROrRgWithZeroBlueAndOpaqueAlphaAndSrgbDecodesWhenSampled)
{
    // Drawn with the unlit shader over the whole 8 x 8 target, white albedo and white vertices: the pixel is the sample
    IMesh* quad = R().CreateMesh(FullQuad());
    ASSERT_NE(quad, nullptr);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto sampleOf = [&](const std::vector<std::uint8_t>& texels, std::uint32_t channels, bool srgb) -> std::vector<std::uint8_t>
    {
        ITexture* texture = R().CreateTexture(texels.data(), 1, 1, channels, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, false, srgb));
        EXPECT_NE(texture, nullptr);
        IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader(), texture);
        EXPECT_NE(material, nullptr);
        R().Clear(0.0f, 0.0f, 0.0f, 0.0f);
        R().BeginFrame();
        R().SetViewProjection(identity, identity);
        R().DrawMesh(quad, identity, material);
        R().EndFrame();
        const std::vector<std::uint8_t> pixels = Read(R(), 8, 8);
        R().DestroyMaterial(material);
        R().DestroyTexture(texture);
        return std::vector<std::uint8_t>(pixels.begin(), pixels.begin() + 4);
    };

    EXPECT_EQ(sampleOf({200}, 1, false), (std::vector<std::uint8_t>{200, 0, 0, 255})) << "R8 samples (r, 0, 0, 1)";
    EXPECT_EQ(sampleOf({50, 100}, 2, false), (std::vector<std::uint8_t>{50, 100, 0, 255})) << "RG8 samples (r, g, 0, 1)";
    EXPECT_EQ(sampleOf({10, 20, 30}, 3, false), (std::vector<std::uint8_t>{10, 20, 30, 255})) << "RGB expanded with alpha 255";
    EXPECT_EQ(sampleOf({10, 20, 30, 40}, 4, false), (std::vector<std::uint8_t>{10, 20, 30, 40}));
    EXPECT_EQ(sampleOf({200}, 1, true), (std::vector<std::uint8_t>{200, 0, 0, 255})) << "1 channel ignores srgb";

    // sRGB: 128 / 255 decodes to 0.2158 linear, the UNORM target stores it as 55 (no encode on write); the alpha is not decoded
    const std::vector<std::uint8_t> decoded = sampleOf({128, 128, 128, 128}, 4, true);
    ASSERT_EQ(decoded.size(), 4u);
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_NEAR(static_cast<int>(decoded[c]), 55, 1) << "channel " << c;
    }
    EXPECT_NEAR(static_cast<int>(decoded[3]), 128, 1) << "alpha is linear";
    const std::vector<std::uint8_t> decodedRgb = sampleOf({128, 128, 128}, 3, true);
    EXPECT_NEAR(static_cast<int>(decodedRgb[0]), 55, 1);
    EXPECT_EQ(decodedRgb[3], 255);
}

// ============================================================================
// Meshes
// ============================================================================

TEST_F(WebGpuResourceTest, CreateMeshWithNoVerticesReturnsNullptr)
{
    MeshData none;
    EXPECT_EQ(R().CreateMesh(none), nullptr);
    MeshData indicesOnly;
    indicesOnly.indices = {0, 1, 2};
    EXPECT_EQ(R().CreateMesh(indicesOnly), nullptr);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
}

TEST_F(WebGpuResourceTest, CreateMeshUploadsTheVerticesAndIndicesExactly)
{
    const MeshData data = MakeMeshData(11, 33, 1);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(mesh->IsValid());
    EXPECT_EQ(mesh->GetVertexCount(), 11u);
    EXPECT_EQ(mesh->GetIndexCount(), 33u);
    const MeshInfoForTest info = R().GetMeshInfoForTest(mesh);
    ASSERT_TRUE(info.valid);
    EXPECT_NE(info.vertexBuffer, nullptr);
    EXPECT_NE(info.indexBuffer, nullptr);
    EXPECT_EQ(info.vertexCount, 11u);
    EXPECT_EQ(info.indexCount, 33u);
    MeshData read;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(data, read));
}

TEST_F(WebGpuResourceTest, AMeshWithVerticesAndNoIndicesIsValidWithoutAnIndexBuffer)
{
    const MeshData data = MakeMeshData(5, 0, 2);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(mesh->IsValid());
    EXPECT_EQ(mesh->GetVertexCount(), 5u);
    EXPECT_EQ(mesh->GetIndexCount(), 0u);
    const MeshInfoForTest info = R().GetMeshInfoForTest(mesh);
    EXPECT_NE(info.vertexBuffer, nullptr);
    EXPECT_EQ(info.indexBuffer, nullptr);
    MeshData read;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(data, read));

    // Updating it to a mesh with indices, and back
    const MeshData withIndices = MakeMeshData(5, 9, 3);
    ASSERT_TRUE(R().UpdateMesh(mesh, withIndices));
    EXPECT_NE(R().GetMeshInfoForTest(mesh).indexBuffer, nullptr);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(withIndices, read));
    ASSERT_TRUE(R().UpdateMesh(mesh, data));
    EXPECT_EQ(R().GetMeshInfoForTest(mesh).indexBuffer, nullptr);
    EXPECT_EQ(mesh->GetIndexCount(), 0u);
}

TEST_F(WebGpuResourceTest, ALargeMeshUploadsExactly)
{
    const MeshData data = MakeMeshData(20000, 60000, 4); // 1.28 MB of vertices
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    MeshData read;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(data, read));
}

TEST_F(WebGpuResourceTest, AMeshOverTheBufferLimitFailsWithoutAWebGpuError)
{
    const std::uint64_t limit = R().GetMaxBufferSizeForTest();
    R().SetMaxBufferSizeForTest(64); // a 4-vertex mesh is 256 bytes
    EXPECT_EQ(R().CreateMesh(MakeMeshData(4, 6, 1)), nullptr);
    R().SetMaxBufferSizeForTest(limit);
    IMesh* mesh = R().CreateMesh(MakeMeshData(4, 6, 1));
    ASSERT_NE(mesh, nullptr);
    R().SetMaxBufferSizeForTest(64);
    EXPECT_FALSE(R().UpdateMesh(mesh, MakeMeshData(8, 6, 2))) << "the bigger data does not fit";
    EXPECT_EQ(mesh->GetVertexCount(), 4u) << "the mesh is unchanged";
    R().SetMaxBufferSizeForTest(limit);
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuResourceTest, UpdateMeshKeepsThePointerAndReplacesTheCountsAndData)
{
    const MeshData first = MakeMeshData(6, 12, 1);
    IMesh* mesh = R().CreateMesh(first);
    ASSERT_NE(mesh, nullptr);
    const std::vector<std::tuple<std::size_t, std::size_t, int>> steps = {{6, 12, 2}, {3, 3, 3}, {40, 120, 4}, {7, 0, 5}, {7, 21, 6}};
    for (const auto& [vertices, indices, seed] : steps)
    {
        const MeshData next = MakeMeshData(vertices, indices, seed);
        ASSERT_TRUE(R().UpdateMesh(mesh, next)) << vertices << " vertices, " << indices << " indices";
        EXPECT_EQ(mesh->GetVertexCount(), vertices);
        EXPECT_EQ(mesh->GetIndexCount(), indices);
        MeshData read;
        ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
        EXPECT_TRUE(SameMesh(next, read)) << vertices << " vertices, " << indices << " indices";
    }
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u);
}

TEST_F(WebGpuResourceTest, UpdateMeshRejectsNullEmptyAndForeignMeshesAndChangesNothing)
{
    const MeshData data = MakeMeshData(4, 6, 1);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    WebGpuRenderer* other = NewInitializedRenderer();
    ASSERT_NE(other, nullptr);
    IMesh* foreign = other->CreateMesh(data);
    ASSERT_NE(foreign, nullptr);

    const MeshData next = MakeMeshData(4, 6, 2);
    EXPECT_FALSE(R().UpdateMesh(nullptr, next));
    EXPECT_FALSE(R().UpdateMesh(foreign, next)) << "another renderer's mesh";
    EXPECT_FALSE(R().UpdateMesh(mesh, MeshData{})) << "no vertices";
    MeshData indicesOnly;
    indicesOnly.indices = {0, 1, 2};
    EXPECT_FALSE(R().UpdateMesh(mesh, indicesOnly));
    MeshData read;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(data, read)) << "nothing changed";
    ASSERT_TRUE(other->ReadMeshForTest(foreign, read));
    EXPECT_TRUE(SameMesh(data, read)) << "the foreign mesh is untouched too";
    EXPECT_FALSE(R().ReadMeshForTest(foreign, read));
    EXPECT_FALSE(R().GetMeshInfoForTest(foreign).valid);
}

TEST_F(WebGpuResourceTest, ForeignAndNullPointersAreIgnoredByDestroyMeshAndDestroyTexture)
{
    const std::vector<std::uint8_t> texels = Pattern(2, 2, 4);
    IMesh* mesh = R().CreateMesh(MakeMeshData(4, 6, 1));
    ITexture* texture = R().CreateTexture(texels.data(), 2, 2, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    WebGpuRenderer* other = NewInitializedRenderer();
    ASSERT_NE(other, nullptr);
    IMesh* foreignMesh = other->CreateMesh(MakeMeshData(4, 6, 1));
    ITexture* foreignTexture = other->CreateTexture(texels.data(), 2, 2, 4);
    ASSERT_NE(foreignMesh, nullptr);
    ASSERT_NE(foreignTexture, nullptr);

    R().DestroyMesh(nullptr);
    R().DestroyTexture(nullptr);
    R().DestroyMesh(foreignMesh);
    R().DestroyTexture(foreignTexture);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 1u);
    EXPECT_EQ(other->GetLiveMeshCountForTest(), 1u) << "the other renderer's mesh was not destroyed by this one";
    EXPECT_EQ(other->GetLiveTextureCountForTest(), 1u);

    // Destroyed twice: the second call is a no-op
    R().DestroyMesh(mesh);
    R().DestroyMesh(mesh);
    R().DestroyTexture(texture);
    R().DestroyTexture(texture);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_FALSE(R().GetMeshInfoForTest(mesh).valid);
    EXPECT_FALSE(R().GetTextureInfoForTest(texture).valid);
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(R().ReadTextureForTest(texture, 0, bytes));
}

// ============================================================================
// UpdateMesh and the frame being recorded
// ============================================================================

TEST_F(WebGpuResourceTest, UpdateMeshWritesInPlaceUnlessTheMeshWasAlreadyDrawnInTheFrameBeingRecorded)
{
    // The rule (docs: webgpu-backend): a queue write executes on the queue's timeline at once, before the command buffer of the
    // frame being recorded is submitted at EndFrame. A mesh already DRAWN in that frame must get new buffers, or the earlier
    // draw would read the new data. The buffer handles are how the test sees which path was taken; the draws' pixels are in
    // WebGpuRendererUnlitDrawTests.cpp
    IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const MeshData a = MakeMeshData(4, 6, 1);
    const MeshData same = MakeMeshData(4, 6, 2);
    const MeshData bigger = MakeMeshData(5, 9, 3);
    IMesh* mesh = R().CreateMesh(a);
    ASSERT_NE(mesh, nullptr);
    MeshData read;

    // Outside a frame with the same counts: written in place
    const MeshInfoForTest created = R().GetMeshInfoForTest(mesh);
    ASSERT_TRUE(R().UpdateMesh(mesh, same));
    MeshInfoForTest info = R().GetMeshInfoForTest(mesh);
    EXPECT_EQ(info.vertexBuffer, created.vertexBuffer);
    EXPECT_EQ(info.indexBuffer, created.indexBuffer);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(same, read));

    // Other counts: new buffers
    ASSERT_TRUE(R().UpdateMesh(mesh, bigger));
    const MeshInfoForTest resized = R().GetMeshInfoForTest(mesh);
    EXPECT_NE(resized.vertexBuffer, info.vertexBuffer);
    EXPECT_NE(resized.indexBuffer, info.indexBuffer);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(bigger, read));

    // Inside a frame, the mesh not drawn in it yet: in place
    const MeshData bigger2 = MakeMeshData(5, 9, 4);
    R().BeginFrame();
    R().SetViewProjection(identity, identity);
    ASSERT_TRUE(R().UpdateMesh(mesh, bigger2));
    info = R().GetMeshInfoForTest(mesh);
    EXPECT_EQ(info.vertexBuffer, resized.vertexBuffer);
    EXPECT_EQ(info.indexBuffer, resized.indexBuffer);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(bigger2, read));

    // Drawn in this frame, then updated with the SAME counts: fresh buffers, and the new data in them
    R().DrawMesh(mesh, identity, material);
    const MeshData bigger3 = MakeMeshData(5, 9, 5);
    ASSERT_TRUE(R().UpdateMesh(mesh, bigger3));
    const MeshInfoForTest fresh = R().GetMeshInfoForTest(mesh);
    EXPECT_NE(fresh.vertexBuffer, info.vertexBuffer) << "the earlier draw keeps the buffer it was recorded with";
    EXPECT_NE(fresh.indexBuffer, info.indexBuffer);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(bigger3, read));
    const MeshData bigger4 = MakeMeshData(5, 9, 6);
    ASSERT_TRUE(R().UpdateMesh(mesh, bigger4)) << "and again in the same frame";
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(bigger4, read));
    R().EndFrame();

    // After the frame is submitted: in place again
    const MeshInfoForTest afterFrame = R().GetMeshInfoForTest(mesh);
    const MeshData bigger5 = MakeMeshData(5, 9, 7);
    ASSERT_TRUE(R().UpdateMesh(mesh, bigger5));
    info = R().GetMeshInfoForTest(mesh);
    EXPECT_EQ(info.vertexBuffer, afterFrame.vertexBuffer);
    EXPECT_EQ(info.indexBuffer, afterFrame.indexBuffer);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(bigger5, read));
}

TEST_F(WebGpuResourceTest, AnotherMeshDrawnInTheFrameDoesNotMakeThisMeshsUpdateAllocate)
{
    // The "drawn this frame" flag is per mesh
    IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    IMesh* drawn = R().CreateMesh(MakeMeshData(4, 6, 1));
    IMesh* idle = R().CreateMesh(MakeMeshData(4, 6, 2));
    ASSERT_NE(drawn, nullptr);
    ASSERT_NE(idle, nullptr);
    const MeshInfoForTest idleBefore = R().GetMeshInfoForTest(idle);
    const MeshInfoForTest drawnBefore = R().GetMeshInfoForTest(drawn);

    R().BeginFrame();
    R().SetViewProjection(identity, identity);
    R().DrawMesh(drawn, identity, material);
    ASSERT_TRUE(R().UpdateMesh(idle, MakeMeshData(4, 6, 3)));
    ASSERT_TRUE(R().UpdateMesh(drawn, MakeMeshData(4, 6, 4)));
    R().EndFrame();

    EXPECT_EQ(R().GetMeshInfoForTest(idle).vertexBuffer, idleBefore.vertexBuffer) << "not drawn in the frame: in place";
    EXPECT_NE(R().GetMeshInfoForTest(drawn).vertexBuffer, drawnBefore.vertexBuffer) << "drawn in the frame: new buffers";
}

// ============================================================================
// Lifetimes
// ============================================================================

TEST_F(WebGpuResourceTest, ResourcesCreatedInsideAnOpenFrameWorkAndTheFrameStillRenders)
{
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    R().BeginFrame();
    R().SetViewProjection(identity, identity);
    IMesh* quad = R().CreateMesh(FullQuad());
    const std::uint8_t texel[4] = {0, 255, 0, 255};
    ITexture* texture = R().CreateTexture(texel, 1, 1, 4, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, false));
    IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader(), texture);
    ASSERT_NE(quad, nullptr);
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(material, nullptr);
    R().DrawMesh(quad, identity, material);
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {0, 255, 0, 255}));
}

TEST_F(WebGpuResourceTest, ResizingTheTargetDoesNotDisturbMeshesAndTextures)
{
    const MeshData data = MakeMeshData(6, 12, 1);
    const std::vector<std::uint8_t> texels = Pattern(9, 7, 4);
    IMesh* mesh = R().CreateMesh(data);
    ITexture* texture = R().CreateTexture(texels.data(), 9, 7, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    for (const std::uint32_t size : {16u, 4u, 33u})
    {
        ASSERT_TRUE(R().SetRenderTargetSize(size, size));
        MeshData read;
        ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
        EXPECT_TRUE(SameMesh(data, read)) << "target " << size;
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
        EXPECT_EQ(bytes, texels) << "target " << size;
    }
}

TEST_F(WebGpuResourceTest, ManyUpdatesAndCreateDestroyCyclesReturnTheLiveObjectCountToItsBaseline)
{
    // The first texture fills the sampler cache, which the renderer keeps: take the baseline after it
    const std::vector<std::uint8_t> texels = Pattern(16, 16, 4);
    ITexture* warmUp = R().CreateTexture(texels.data(), 16, 16, 4);
    ASSERT_NE(warmUp, nullptr);
    R().DestroyTexture(warmUp);
    const std::int64_t baseline = WebGpuRenderer::GetLiveObjectCountForTest();

    for (int cycle = 0; cycle < 50; ++cycle)
    {
        IMesh* mesh = R().CreateMesh(MakeMeshData(8, 12, cycle));
        ITexture* texture = R().CreateTexture(texels.data(), 16, 16, 4);
        IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader(), texture);
        ASSERT_NE(mesh, nullptr);
        ASSERT_NE(texture, nullptr);
        ASSERT_NE(material, nullptr);
        EXPECT_TRUE(R().UpdateMesh(mesh, MakeMeshData(8, 12, cycle + 100))); // in place
        EXPECT_TRUE(R().UpdateMesh(mesh, MakeMeshData(9, 15, cycle + 200))); // new buffers
        R().DestroyMaterial(material);
        R().DestroyTexture(texture);
        R().DestroyMesh(mesh);
    }
    EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), baseline) << "references leaked over 50 create/update/destroy cycles";
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
}

TEST_F(WebGpuResourceTest, TenRenderersEachWithResourcesAndADrawLeakNothing)
{
    const std::int64_t liveBefore = WebGpuRenderer::GetLiveObjectCountForTest();
    const std::uint8_t texel[4] = {10, 20, 30, 255};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        {
            WebGpuRenderer renderer;
            ASSERT_TRUE(renderer.Initialize(nullptr, 16, 16)) << "cycle " << cycle << ": " << renderer.GetInitMessage();
            IMesh* mesh = renderer.CreateMesh(FullQuad());
            ITexture* texture = renderer.CreateTexture(texel, 1, 1, 4);
            IMaterial* material = renderer.CreateMaterial(renderer.GetStandardUnlitShader(), texture);
            ASSERT_NE(material, nullptr);
            renderer.BeginFrame();
            renderer.SetViewProjection(identity, identity);
            renderer.DrawMesh(mesh, identity, material);
            renderer.EndFrame();
            EXPECT_EQ(renderer.GetValidationErrorCount(), 0u) << "cycle " << cycle;
            renderer.BeginFrame(); // a frame with a draw left open at destruction
            renderer.DrawMesh(mesh, identity, material);
        } // destroyed with resources alive and a frame open
        EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), liveBefore) << "references leaked in cycle " << cycle;
    }
}

TEST_F(WebGpuResourceTest, ResourcesNeverDestroyedAreFreedAtShutdownWithNoError)
{
    const std::int64_t liveAtStart = WebGpuRenderer::GetLiveObjectCountForTest();
    WebGpuRenderer& renderer = NewRenderer();
    ASSERT_TRUE(renderer.Initialize(nullptr, 8, 8));
    const std::vector<std::uint8_t> texels = Pattern(8, 8, 4);
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_NE(renderer.CreateMesh(MakeMeshData(4, 6, i)), nullptr);
        ASSERT_NE(renderer.CreateTexture(texels.data(), 8, 8, 4), nullptr);
    }
    ASSERT_NE(renderer.CreateMaterial(renderer.GetStandardUnlitShader()), nullptr);
    EXPECT_EQ(renderer.GetLiveMeshCountForTest(), 5u);
    EXPECT_EQ(renderer.GetLiveTextureCountForTest(), 5u);
    EXPECT_EQ(renderer.GetLiveMaterialCountForTest(), 1u);
    renderer.Shutdown();
    EXPECT_EQ(renderer.GetValidationErrorCount(), 0u);
    EXPECT_EQ(renderer.GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(renderer.GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(renderer.GetLiveMaterialCountForTest(), 0u);
    // Only the fixture's own renderer still holds references
    EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), liveAtStart);
}

TEST_F(WebGpuResourceTest, DestroyAndUpdateAfterShutdownAreHarmlessAndCreatesFail)
{
    const std::vector<std::uint8_t> texels = Pattern(2, 2, 4);
    IMesh* mesh = R().CreateMesh(MakeMeshData(4, 6, 1));
    ITexture* texture = R().CreateTexture(texels.data(), 2, 2, 4);
    IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader());
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(material, nullptr);
    R().Shutdown();

    R().DestroyMesh(mesh);
    R().DestroyTexture(texture);
    R().DestroyMaterial(material);
    EXPECT_FALSE(R().UpdateMesh(mesh, MakeMeshData(4, 6, 2)));
    EXPECT_EQ(R().CreateMesh(MakeMeshData(4, 6, 3)), nullptr);
    EXPECT_EQ(R().CreateTexture(texels.data(), 2, 2, 4), nullptr);
    EXPECT_EQ(R().CreateMaterial(nullptr), nullptr);
    EXPECT_EQ(R().GetStandardUnlitShader(), nullptr);
    R().BeginFrame();
    R().DrawMesh(mesh, nullptr, material);
    R().EndFrame();
    std::vector<std::uint8_t> bytes;
    MeshData read;
    EXPECT_FALSE(R().ReadTextureForTest(texture, 0, bytes));
    EXPECT_FALSE(R().ReadMeshForTest(mesh, read));
}

TEST_F(WebGpuResourceTest, ResourceCallsOnARendererThatWasNeverInitializedReturnNullptr)
{
    WebGpuRenderer& fresh = NewRenderer();
    const std::uint8_t texel[4] = {1, 2, 3, 4};
    EXPECT_EQ(fresh.CreateMesh(MakeMeshData(4, 6, 1)), nullptr);
    EXPECT_EQ(fresh.CreateTexture(texel, 1, 1, 4), nullptr);
    EXPECT_EQ(fresh.GetStandardUnlitShader(), nullptr);
    EXPECT_EQ(fresh.CreateMaterial(R().GetStandardUnlitShader()), nullptr) << "a shader of another renderer";
    EXPECT_FALSE(fresh.IsValidShader(R().GetStandardUnlitShader()));
    fresh.DestroyMesh(nullptr);
    fresh.DestroyTexture(nullptr);
    fresh.DestroyMaterial(nullptr);
    fresh.SetViewProjection(nullptr, nullptr);
    fresh.BeginFrame();
    fresh.DrawMesh(nullptr, nullptr, nullptr);
    fresh.EndFrame();
}

TEST_F(WebGpuResourceTest, AfterShutdownTheRendererCanBeInitializedAgainAndCreateResourcesAgain)
{
    const std::vector<std::uint8_t> texels = Pattern(3, 3, 4);
    IMesh* old = R().CreateMesh(MakeMeshData(4, 6, 1));
    ASSERT_NE(old, nullptr);
    R().Shutdown();
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();
    EXPECT_FALSE(R().GetMeshInfoForTest(old).valid) << "the old mesh is not the new renderer's";
    const MeshData data = MakeMeshData(5, 9, 2);
    IMesh* mesh = R().CreateMesh(data);
    ITexture* texture = R().CreateTexture(texels.data(), 3, 3, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    MeshData read;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, read));
    EXPECT_TRUE(SameMesh(data, read));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
    EXPECT_EQ(bytes, texels);
}

TEST_F(WebGpuResourceTest, InitializingAgainWithoutShutdownFreesTheOldResources)
{
    const std::vector<std::uint8_t> texels = Pattern(8, 8, 4);
    const std::int64_t baseline = WebGpuRenderer::GetLiveObjectCountForTest();
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_NE(R().CreateMesh(MakeMeshData(4, 6, i)), nullptr);
        ASSERT_NE(R().CreateTexture(texels.data(), 8, 8, 4), nullptr);
    }
    EXPECT_GT(WebGpuRenderer::GetLiveObjectCountForTest(), baseline);
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), baseline) << "the old device and every resource went";
}

// ============================================================================
// Device loss
// ============================================================================

TEST_F(WebGpuResourceTest, ALostDeviceFailsNewCallsCleanlyAndANewInitializeExpiresTheTokenSoOwnersRecreateTheirResources)
{
    // The WebGPU analogue of the Vulkan backend's device loss: nothing here recovers the device by itself. The owners of
    // resources (GpuCache) hold the renderer's lifetime token; Initialize on a lost renderer ends the old lifetime, so every
    // entry made with the old token is replaced by a new resource the next time it is acquired. Dawn's device destroy is the
    // injection (the same one the G2 loss test uses)
    const std::vector<std::uint8_t> texels = Pattern(4, 4, 4);
    IMesh* mesh = R().CreateMesh(MakeMeshData(4, 6, 1));
    ITexture* texture = R().CreateTexture(texels.data(), 4, 4, 4);
    IMaterial* material = R().CreateMaterial(R().GetStandardUnlitShader(), texture);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(material, nullptr);
    const std::weak_ptr<const void> token = R().GetLifetimeToken();
    ASSERT_FALSE(token.expired());
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    wgpuDeviceDestroy(R().GetDeviceForTest());
    EXPECT_GE(R().GetValidationErrorCount(), 1u) << "the loss is counted";
    ASSERT_TRUE(R().IsDeviceLost());
    EXPECT_FALSE(token.expired()) << "the renderer object still exists: its owners learn of the loss from the next Initialize";

    // New resources fail, draws and frames are skipped, destroying what was there is harmless
    EXPECT_EQ(R().CreateMesh(MakeMeshData(4, 6, 2)), nullptr);
    EXPECT_EQ(R().CreateTexture(texels.data(), 4, 4, 4), nullptr);
    EXPECT_EQ(R().CreateMaterial(R().GetStandardUnlitShader()), nullptr);
    EXPECT_FALSE(R().UpdateMesh(mesh, MakeMeshData(4, 6, 3)));
    R().BeginFrame();
    R().SetViewProjection(identity, identity);
    R().DrawMesh(mesh, identity, material);
    R().EndFrame();
    R().DestroyMaterial(material);
    R().DestroyTexture(texture);
    R().DestroyMesh(mesh);

    // A new Initialize makes a new device: the token expires, the old pointers are foreign, new resources work
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();
    EXPECT_TRUE(token.expired()) << "everything holding the old token must make its resources again";
    EXPECT_FALSE(R().IsDeviceLost());
    EXPECT_FALSE(R().GetMeshInfoForTest(mesh).valid);
    IMesh* fresh = R().CreateMesh(FullQuad());
    IMaterial* freshMaterial = R().CreateMaterial(R().GetStandardUnlitShader());
    ASSERT_NE(fresh, nullptr);
    ASSERT_NE(freshMaterial, nullptr);
    freshMaterial->SetColor("uAlbedo", 0.0f, 0.0f, 1.0f, 1.0f);
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    R().BeginFrame();
    R().SetViewProjection(identity, identity);
    R().DrawMesh(fresh, identity, freshMaterial);
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {0, 0, 255, 255}));

    R().ResetValidationErrors(); // this test provoked them on purpose
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuResourceTest, TheLifetimeTokenOfARendererExpiresOnShutdownAndOnInitializeAgain)
{
    const std::weak_ptr<const void> first = R().GetLifetimeToken();
    EXPECT_FALSE(first.expired());
    R().Shutdown();
    EXPECT_TRUE(first.expired());
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();
    const std::weak_ptr<const void> second = R().GetLifetimeToken();
    EXPECT_FALSE(second.expired());
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage(); // again, without a Shutdown
    EXPECT_TRUE(second.expired());
}
