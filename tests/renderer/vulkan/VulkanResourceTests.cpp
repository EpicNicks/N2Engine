#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RendererConfig.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanTestFixture.hpp"

// The Vulkan backend's resources (#43, stage A): meshes, textures, samplers, staging uploads, blit mipmaps, deferred
// deletion and UpdateMesh, run headless with validation ON. Nothing draws until stage B, so the tests prove what was
// uploaded through the test hooks (ReadTextureForTest, ReadMeshForTest, GetTextureInfoForTest) and what was freed
// through the allocation counters (GetLiveAllocationCountForTest, GetPendingDeletionCountForTest). Like the stage 0
// tests they execute on Mesa lavapipe in the Linux Clang CI legs (N2_REQUIRE_VULKAN=1 makes a skip a failure) and skip
// where there is no Vulkan device. The fixture's teardown fails any test during which the validation layer reported an
// error or a warning, a leaked object included.

using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::Vertex;
using Renderer::Vulkan::TextureInfoForTest;
using Renderer::Vulkan::VulkanRenderer;
using namespace VulkanTesting;

namespace
{
    class VulkanResourceTest : public VulkanTest
    {
    protected:
        /// A second initialized renderer, whose resources are foreign to R(); skips like SetUp when there is no device
        VulkanRenderer* NewInitializedRenderer()
        {
            VulkanRenderer& renderer = NewRenderer();
            if (!renderer.Initialize(nullptr, 8, 8))
            {
                HandleInitFailure(renderer);
                return nullptr;
            }
            return &renderer;
        }

        static void Frame(VulkanRenderer& renderer)
        {
            renderer.BeginFrame();
            renderer.EndFrame();
        }
    };

    /// A vertex whose every float is distinct (a function of `seed` and the slot), so a copy that drops, reorders or
    /// shifts anything shows
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

    MeshData MakeMesh(std::size_t vertexCount, std::size_t indexCount, int seed)
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

    bool SameVertices(const MeshData& a, const MeshData& b)
    {
        return a.vertices.size() == b.vertices.size() &&
            (a.vertices.empty() ||
             std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0);
    }

    ::testing::AssertionResult SameMesh(const MeshData& expected, const MeshData& actual)
    {
        if (!SameVertices(expected, actual))
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
                        static_cast<std::uint8_t>((37 * x + 101 * y + 53 * c + 11 + static_cast<std::uint32_t>(seed)) & 0xFF);
                }
            }
        }
        return bytes;
    }

    /// What the texture's level 0 holds: the input as given, except that 3 channels become RGBA with alpha 255
    std::vector<std::uint8_t> ExpectedLevel0(const std::vector<std::uint8_t>& input, std::uint32_t width,
                                             std::uint32_t height, std::uint32_t channels)
    {
        if (channels != 3)
        {
            return input;
        }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i)
        {
            out[i * 4 + 0] = input[i * 3 + 0];
            out[i * 4 + 1] = input[i * 3 + 1];
            out[i * 4 + 2] = input[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }
        return out;
    }

    std::uint32_t FullMipCount(std::uint32_t width, std::uint32_t height)
    {
        std::uint32_t levels = 1;
        for (std::uint32_t size = std::max(width, height); size > 1; size >>= 1)
        {
            ++levels;
        }
        return levels;
    }

    bool RunningOnTheCpuDevice()
    {
        return Env("N2_VULKAN_EXPECT_CPU_DEVICE") == "1";
    }

    class FakeMesh final : public IMesh
    {
    public:
        bool IsValid() const override { return true; }
        std::uint32_t GetIndexCount() const override { return 3; }
        std::uint32_t GetVertexCount() const override { return 3; }
    };

    class FakeTexture final : public ITexture
    {
    public:
        bool IsValid() const override { return true; }
        std::uint32_t GetWidth() const override { return 1; }
        std::uint32_t GetHeight() const override { return 1; }
        std::uint32_t GetChannels() const override { return 4; }
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures: what is rejected
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, TextureCreationRejectsNullDataZeroSizeAndBadChannelCounts)
{
    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    EXPECT_EQ(R().CreateTexture(nullptr, 4, 4, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(nullptr, 4, 4, 4, TextureOptions{}), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 0, 4, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 0, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 0, 0, 4, TextureOptions{}), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 4, 0), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 4, 5), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 4, 5, TextureOptions{}), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 4, std::numeric_limits<std::uint32_t>::max()), nullptr);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, TextureCreationRejectsASizeOverTheDeviceLimit)
{
    const std::uint8_t texel[4] = {1, 2, 3, 4};
    // Rejected before any pixel is read, so the tiny buffer is never touched
    EXPECT_EQ(R().CreateTexture(texel, std::numeric_limits<std::uint32_t>::max(), 1, 4), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1, std::numeric_limits<std::uint32_t>::max(), 4, TextureOptions{}), nullptr);
    EXPECT_EQ(R().CreateTexture(texel, 1u << 30, 1u << 30, 1), nullptr);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures: every channel count and option, exact bytes
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, EveryChannelCountAndOptionCreatesATextureWhoseLevelZeroHoldsTheExactBytes)
{
    struct Size
    {
        std::uint32_t w, h;
    };
    const Size sizes[] = {{1, 1}, {7, 5}, {5, 9}}; // odd and non-power-of-two, 3-channel rows of 15 and 21 bytes

    for (std::uint32_t channels = 1; channels <= 4; ++channels)
    {
        for (const bool srgb : {false, true})
        {
            for (const TextureFilter filter : {TextureFilter::Nearest, TextureFilter::Linear})
            {
                for (const TextureWrap wrap : {TextureWrap::Repeat, TextureWrap::ClampToEdge})
                {
                    for (const bool mipmaps : {false, true})
                    {
                        for (const Size size : sizes)
                        {
                            SCOPED_TRACE(testing::Message()
                                         << channels << " channels, srgb " << srgb << ", filter "
                                         << static_cast<int>(filter) << ", wrap " << static_cast<int>(wrap)
                                         << ", mipmaps " << mipmaps << ", " << size.w << "x" << size.h);
                            TextureOptions options;
                            options.filter = filter;
                            options.wrap = wrap;
                            options.mipmaps = mipmaps;
                            options.srgb = srgb;
                            const std::vector<std::uint8_t> input = Pattern(size.w, size.h, channels, 3);

                            ITexture* texture = R().CreateTexture(input.data(), size.w, size.h, channels, options);
                            ASSERT_NE(texture, nullptr);
                            EXPECT_TRUE(texture->IsValid());
                            EXPECT_EQ(texture->GetWidth(), size.w);
                            EXPECT_EQ(texture->GetHeight(), size.h);
                            EXPECT_EQ(texture->GetChannels(), channels); // as given, whatever it is stored as

                            const TextureInfoForTest info = R().GetTextureInfoForTest(texture);
                            ASSERT_TRUE(info.valid);
                            EXPECT_EQ(info.width, size.w);
                            EXPECT_EQ(info.height, size.h);
                            EXPECT_EQ(info.channels, channels);

                            // The format follows the channel count; sRGB only for 3 and 4 channels
                            if (channels == 1)
                            {
                                EXPECT_EQ(info.format, VK_FORMAT_R8_UNORM);
                            }
                            else if (channels == 2)
                            {
                                EXPECT_EQ(info.format, VK_FORMAT_R8G8_UNORM);
                            }
                            else if (srgb)
                            {
                                // The fallback to UNORM is for a device that cannot sample RGBA8 sRGB; lavapipe can
                                if (RunningOnTheCpuDevice())
                                {
                                    EXPECT_EQ(info.format, VK_FORMAT_R8G8B8A8_SRGB);
                                }
                                else
                                {
                                    EXPECT_TRUE(info.format == VK_FORMAT_R8G8B8A8_SRGB ||
                                                info.format == VK_FORMAT_R8G8B8A8_UNORM);
                                }
                            }
                            else
                            {
                                EXPECT_EQ(info.format, VK_FORMAT_R8G8B8A8_UNORM);
                            }

                            // Mip levels: one, or the whole chain when asked for (and, on a device that cannot blit
                            // linearly, one; lavapipe can)
                            if (!mipmaps)
                            {
                                EXPECT_EQ(info.mipLevels, 1u);
                            }
                            else if (RunningOnTheCpuDevice())
                            {
                                EXPECT_EQ(info.mipLevels, FullMipCount(size.w, size.h));
                            }
                            else
                            {
                                EXPECT_TRUE(info.mipLevels == 1u || info.mipLevels == FullMipCount(size.w, size.h));
                            }

                            // The sampler follows the options
                            EXPECT_EQ(info.filter, filter == TextureFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
                            EXPECT_EQ(info.mipmapMode, filter == TextureFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST);
                            EXPECT_EQ(info.addressMode, wrap == TextureWrap::Repeat
                                                            ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                            : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
                            EXPECT_TRUE(info.sampler != VK_NULL_HANDLE);

                            // Level 0, byte for byte: 3 channels are expanded to RGBA with alpha 255, the rest are as given
                            std::vector<std::uint8_t> level0;
                            std::uint32_t levelWidth = 0;
                            std::uint32_t levelHeight = 0;
                            ASSERT_TRUE(R().ReadTextureForTest(texture, 0, level0, &levelWidth, &levelHeight));
                            EXPECT_EQ(levelWidth, size.w);
                            EXPECT_EQ(levelHeight, size.h);
                            EXPECT_EQ(level0, ExpectedLevel0(input, size.w, size.h, channels));

                            R().DestroyTexture(texture);
                        }
                    }
                }
            }
        }
    }
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, ThreeChannelTexturesAreExpandedToRgbaWithAlpha255AndRowsAreTightlyPacked)
{
    // Width 5 is not a multiple of 4: a driver-aligned row would shift every row after the first
    const std::uint32_t w = 5;
    const std::uint32_t h = 3;
    std::vector<std::uint8_t> rgb;
    for (std::uint32_t i = 0; i < w * h; ++i)
    {
        rgb.push_back(static_cast<std::uint8_t>(i * 10));
        rgb.push_back(static_cast<std::uint8_t>(i * 10 + 1));
        rgb.push_back(static_cast<std::uint8_t>(i * 10 + 2));
    }
    TextureOptions options;
    options.mipmaps = false;
    ITexture* texture = R().CreateTexture(rgb.data(), w, h, 3, options);
    ASSERT_NE(texture, nullptr);

    std::vector<std::uint8_t> level0;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 0, level0));
    ASSERT_EQ(level0.size(), static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t i = 0; i < w * h; ++i)
    {
        EXPECT_EQ(level0[i * 4 + 0], i * 10) << "pixel " << i;
        EXPECT_EQ(level0[i * 4 + 1], i * 10 + 1) << "pixel " << i;
        EXPECT_EQ(level0[i * 4 + 2], i * 10 + 2) << "pixel " << i;
        EXPECT_EQ(level0[i * 4 + 3], 255) << "pixel " << i;
    }
}

TEST_F(VulkanResourceTest, OneAndTwoChannelTexturesKeepTheirDataAndSampleAsROrRgWithZeroBlueAndOpaqueAlpha)
{
    const std::vector<std::uint8_t> one = Pattern(6, 3, 1, 9);
    const std::vector<std::uint8_t> two = Pattern(6, 3, 2, 9);
    TextureOptions options;
    options.mipmaps = false;
    ITexture* t1 = R().CreateTexture(one.data(), 6, 3, 1, options);
    ITexture* t2 = R().CreateTexture(two.data(), 6, 3, 2, options);
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(R().ReadTextureForTest(t1, 0, bytes));
    EXPECT_EQ(bytes, one); // one byte per texel
    ASSERT_TRUE(R().ReadTextureForTest(t2, 0, bytes));
    EXPECT_EQ(bytes, two); // two bytes per texel, r then g

    // (r,0,0,1) and (r,g,0,1), as OpenGL and the software renderer sample them
    const TextureInfoForTest i1 = R().GetTextureInfoForTest(t1);
    EXPECT_EQ(i1.swizzle.r, VK_COMPONENT_SWIZZLE_R);
    EXPECT_EQ(i1.swizzle.g, VK_COMPONENT_SWIZZLE_ZERO);
    EXPECT_EQ(i1.swizzle.b, VK_COMPONENT_SWIZZLE_ZERO);
    EXPECT_EQ(i1.swizzle.a, VK_COMPONENT_SWIZZLE_ONE);
    const TextureInfoForTest i2 = R().GetTextureInfoForTest(t2);
    EXPECT_EQ(i2.swizzle.r, VK_COMPONENT_SWIZZLE_R);
    EXPECT_EQ(i2.swizzle.g, VK_COMPONENT_SWIZZLE_G);
    EXPECT_EQ(i2.swizzle.b, VK_COMPONENT_SWIZZLE_ZERO);
    EXPECT_EQ(i2.swizzle.a, VK_COMPONENT_SWIZZLE_ONE);
}

TEST_F(VulkanResourceTest, ThreeAndFourChannelTexturesSampleAllFourChannelsAsStored)
{
    const std::vector<std::uint8_t> rgb = Pattern(3, 3, 3);
    const std::vector<std::uint8_t> rgba = Pattern(3, 3, 4);
    ITexture* t3 = R().CreateTexture(rgb.data(), 3, 3, 3);
    ITexture* t4 = R().CreateTexture(rgba.data(), 3, 3, 4);
    ASSERT_NE(t3, nullptr);
    ASSERT_NE(t4, nullptr);
    for (const ITexture* t : {t3, t4})
    {
        const TextureInfoForTest info = R().GetTextureInfoForTest(t);
        EXPECT_EQ(info.swizzle.r, VK_COMPONENT_SWIZZLE_IDENTITY);
        EXPECT_EQ(info.swizzle.g, VK_COMPONENT_SWIZZLE_IDENTITY);
        EXPECT_EQ(info.swizzle.b, VK_COMPONENT_SWIZZLE_IDENTITY);
        EXPECT_EQ(info.swizzle.a, VK_COMPONENT_SWIZZLE_IDENTITY);
    }
}

TEST_F(VulkanResourceTest, TheOverloadWithoutOptionsUsesTheDefaults)
{
    const std::vector<std::uint8_t> pixels = Pattern(8, 8, 4);
    ITexture* plain = R().CreateTexture(pixels.data(), 8, 8, 4);
    ITexture* explicitDefault = R().CreateTexture(pixels.data(), 8, 8, 4, TextureOptions::Default());
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(explicitDefault, nullptr);
    const TextureInfoForTest a = R().GetTextureInfoForTest(plain);
    const TextureInfoForTest b = R().GetTextureInfoForTest(explicitDefault);
    EXPECT_EQ(a.format, b.format);
    EXPECT_EQ(a.mipLevels, b.mipLevels);
    EXPECT_EQ(a.filter, VK_FILTER_LINEAR);
    EXPECT_EQ(a.addressMode, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    EXPECT_EQ(a.sampler, b.sampler); // equal options share one cached sampler
}

TEST_F(VulkanResourceTest, SamplersAreCachedByOptionsAndDifferentOptionsGetDifferentSamplers)
{
    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    TextureOptions linearRepeat;
    TextureOptions nearestRepeat;
    nearestRepeat.filter = TextureFilter::Nearest;
    TextureOptions linearClamp;
    linearClamp.wrap = TextureWrap::ClampToEdge;

    ITexture* a = R().CreateTexture(pixels.data(), 4, 4, 4, linearRepeat);
    ITexture* b = R().CreateTexture(pixels.data(), 4, 4, 4, linearRepeat);
    ITexture* c = R().CreateTexture(pixels.data(), 4, 4, 4, nearestRepeat);
    ITexture* d = R().CreateTexture(pixels.data(), 4, 4, 4, linearClamp);
    ASSERT_TRUE(a && b && c && d);
    const VkSampler sa = R().GetTextureInfoForTest(a).sampler;
    EXPECT_EQ(sa, R().GetTextureInfoForTest(b).sampler);
    EXPECT_NE(sa, R().GetTextureInfoForTest(c).sampler);
    EXPECT_NE(sa, R().GetTextureInfoForTest(d).sampler);
    EXPECT_NE(R().GetTextureInfoForTest(c).sampler, R().GetTextureInfoForTest(d).sampler);

    // The sampler outlives the textures that used it: a destroyed texture does not take it away from the others
    R().DestroyTexture(a);
    EXPECT_EQ(R().GetTextureInfoForTest(b).sampler, sa);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures: mipmaps
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, MipmapsOffGivesExactlyOneLevelAndLevelsBeyondItAreNotReadable)
{
    const std::vector<std::uint8_t> pixels = Pattern(16, 16, 4);
    TextureOptions options;
    options.mipmaps = false;
    ITexture* texture = R().CreateTexture(pixels.data(), 16, 16, 4, options);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(R().GetTextureInfoForTest(texture).mipLevels, 1u);
    std::vector<std::uint8_t> bytes;
    EXPECT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
    EXPECT_FALSE(R().ReadTextureForTest(texture, 1, bytes));
    EXPECT_TRUE(bytes.empty());
}

TEST_F(VulkanResourceTest, BlitMipLevelsHaveTheExpectedSizesAndAveragedValues)
{
    // A single 1x1 texel has one level whatever is asked for; the chain of the others ends at 1x1
    struct Case
    {
        std::uint32_t w, h;
    };
    const Case cases[] = {{1, 1}, {2, 2}, {8, 8}, {16, 2}, {7, 5}, {33, 17}};
    for (const Case c : cases)
    {
        SCOPED_TRACE(testing::Message() << c.w << "x" << c.h);
        const std::vector<std::uint8_t> pixels = Pattern(c.w, c.h, 4, 5);
        TextureOptions options;
        options.mipmaps = true;
        ITexture* texture = R().CreateTexture(pixels.data(), c.w, c.h, 4, options);
        ASSERT_NE(texture, nullptr);
        const std::uint32_t levels = R().GetTextureInfoForTest(texture).mipLevels;
        if (RunningOnTheCpuDevice())
        {
            ASSERT_EQ(levels, FullMipCount(c.w, c.h)); // lavapipe can blit linearly: the whole chain
        }
        ASSERT_GE(levels, 1u);
        ASSERT_LE(levels, FullMipCount(c.w, c.h));

        // The range of every channel in level 0: an averaged level stays inside it
        std::uint8_t low[4] = {255, 255, 255, 255};
        std::uint8_t high[4] = {0, 0, 0, 0};
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            low[i % 4] = std::min(low[i % 4], pixels[i]);
            high[i % 4] = std::max(high[i % 4], pixels[i]);
        }

        for (std::uint32_t level = 0; level < levels; ++level)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t lw = 0;
            std::uint32_t lh = 0;
            ASSERT_TRUE(R().ReadTextureForTest(texture, level, bytes, &lw, &lh)) << "level " << level;
            EXPECT_EQ(lw, std::max(1u, c.w >> level)) << "level " << level;
            EXPECT_EQ(lh, std::max(1u, c.h >> level)) << "level " << level;
            ASSERT_EQ(bytes.size(), static_cast<std::size_t>(lw) * lh * 4) << "level " << level;
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                EXPECT_GE(bytes[i], low[i % 4] > 0 ? low[i % 4] - 1 : 0) << "level " << level << " byte " << i;
                EXPECT_LE(bytes[i], high[i % 4] < 255 ? high[i % 4] + 1 : 255) << "level " << level << " byte " << i;
            }
        }
        std::vector<std::uint8_t> none;
        EXPECT_FALSE(R().ReadTextureForTest(texture, levels, none)); // one past the last level
        R().DestroyTexture(texture);
    }
}

TEST_F(VulkanResourceTest, ABlitMipLevelOfAnExactTwoToOneReductionIsTheAverageOfEachTwoByTwoBlock)
{
    const std::uint32_t w = 4;
    const std::uint32_t h = 4;
    const std::vector<std::uint8_t> pixels = Pattern(w, h, 1, 0);
    TextureOptions options;
    options.mipmaps = true;
    ITexture* texture = R().CreateTexture(pixels.data(), w, h, 1, options);
    ASSERT_NE(texture, nullptr);
    if (R().GetTextureInfoForTest(texture).mipLevels < 2)
    {
        ASSERT_FALSE(RunningOnTheCpuDevice()) << "lavapipe blits linearly: expected a mip chain";
        GTEST_SKIP() << "this device cannot blit this format linearly: single level, as designed";
    }

    std::vector<std::uint8_t> level1;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 1, level1));
    ASSERT_EQ(level1.size(), 4u);
    for (std::uint32_t y = 0; y < 2; ++y)
    {
        for (std::uint32_t x = 0; x < 2; ++x)
        {
            const int sum = pixels[(y * 2) * w + x * 2] + pixels[(y * 2) * w + x * 2 + 1] +
                pixels[(y * 2 + 1) * w + x * 2] + pixels[(y * 2 + 1) * w + x * 2 + 1];
            const int expected = (sum + 2) / 4;
            EXPECT_NEAR(static_cast<int>(level1[y * 2 + x]), expected, 2) << "texel " << x << "," << y;
        }
    }

    // And a constant colour stays exactly constant down the whole chain
    const std::vector<std::uint8_t> flat(8 * 8 * 4, 0x80);
    ITexture* flatTexture = R().CreateTexture(flat.data(), 8, 8, 4, options);
    ASSERT_NE(flatTexture, nullptr);
    const std::uint32_t levels = R().GetTextureInfoForTest(flatTexture).mipLevels;
    for (std::uint32_t level = 0; level < levels; ++level)
    {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(R().ReadTextureForTest(flatTexture, level, bytes));
        for (const std::uint8_t b : bytes)
        {
            ASSERT_EQ(b, 0x80) << "level " << level;
        }
    }
}

TEST_F(VulkanResourceTest, ALargeTextureGetsTheWholeMipChain)
{
    const std::vector<std::uint8_t> pixels = Pattern(256, 256, 4);
    ITexture* texture = R().CreateTexture(pixels.data(), 256, 256, 4);
    ASSERT_NE(texture, nullptr);
    const TextureInfoForTest info = R().GetTextureInfoForTest(texture);
    if (RunningOnTheCpuDevice())
    {
        EXPECT_EQ(info.mipLevels, 9u);
    }
    std::vector<std::uint8_t> level0;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 0, level0));
    EXPECT_EQ(level0, pixels);
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, CreateMeshWithNoVerticesReturnsNullptr)
{
    MeshData empty;
    EXPECT_EQ(R().CreateMesh(empty), nullptr);
    empty.indices = {0, 1, 2}; // indices without vertices are still no mesh
    EXPECT_EQ(R().CreateMesh(empty), nullptr);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, CreateMeshUploadsTheVerticesAndIndicesExactly)
{
    const MeshData data = MakeMesh(5, 9, 1);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(mesh->IsValid());
    EXPECT_EQ(mesh->GetVertexCount(), 5u);
    EXPECT_EQ(mesh->GetIndexCount(), 9u);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u); // a vertex buffer and an index buffer

    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));
}

TEST_F(VulkanResourceTest, AMeshWithVerticesAndNoIndicesIsValidWithoutAnIndexBuffer)
{
    const MeshData data = MakeMesh(3, 0, 2);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_EQ(mesh->GetIndexCount(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 1u);
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));

    // Updating to indexed data adds the index buffer, and back again removes it
    const MeshData indexed = MakeMesh(4, 6, 3);
    ASSERT_TRUE(R().UpdateMesh(mesh, indexed));
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(indexed, back));
    ASSERT_TRUE(R().UpdateMesh(mesh, data));
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));
}

TEST_F(VulkanResourceTest, ALargeMeshUploadsExactly)
{
    const MeshData data = MakeMesh(20000, 60000, 4); // 1.28 MB of vertices
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));
}

TEST_F(VulkanResourceTest, UpdateMeshKeepsThePointerAndReplacesTheCountsAndData)
{
    const MeshData first = MakeMesh(3, 3, 1);
    IMesh* mesh = R().CreateMesh(first);
    ASSERT_NE(mesh, nullptr);

    const MeshData second = MakeMesh(8, 12, 2);
    EXPECT_TRUE(R().UpdateMesh(mesh, second));
    EXPECT_TRUE(mesh->IsValid());
    EXPECT_EQ(mesh->GetVertexCount(), 8u);
    EXPECT_EQ(mesh->GetIndexCount(), 12u);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u); // the same mesh, not a new one
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(second, back));

    const MeshData third = MakeMesh(2, 3, 3); // shrinking works too
    EXPECT_TRUE(R().UpdateMesh(mesh, third));
    EXPECT_EQ(mesh->GetVertexCount(), 2u);
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(third, back));
}

TEST_F(VulkanResourceTest, UpdateMeshRejectsNullEmptyAndForeignMeshesAndChangesNothing)
{
    const MeshData data = MakeMesh(4, 6, 1);
    IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    const std::uint32_t allocations = R().GetLiveAllocationCountForTest();

    const MeshData other = MakeMesh(5, 3, 2);
    EXPECT_FALSE(R().UpdateMesh(nullptr, other));

    MeshData noVertices;
    noVertices.indices = {0, 1, 2};
    EXPECT_FALSE(R().UpdateMesh(mesh, noVertices));
    EXPECT_FALSE(R().UpdateMesh(mesh, MeshData{}));

    FakeMesh fake;
    EXPECT_FALSE(R().UpdateMesh(&fake, other));

    if (VulkanRenderer* second = NewInitializedRenderer())
    {
        IMesh* foreign = second->CreateMesh(other);
        ASSERT_NE(foreign, nullptr);
        EXPECT_FALSE(R().UpdateMesh(foreign, data)); // another renderer's mesh
        MeshData foreignBack;
        ASSERT_TRUE(second->ReadMeshForTest(foreign, foreignBack));
        EXPECT_TRUE(SameMesh(other, foreignBack)); // and it was not touched
        EXPECT_FALSE(second->UpdateMesh(mesh, data)); // R()'s mesh is foreign to the second renderer
    }

    // Nothing changed
    EXPECT_EQ(mesh->GetVertexCount(), 4u);
    EXPECT_EQ(mesh->GetIndexCount(), 6u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), allocations);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));
}

TEST_F(VulkanResourceTest, ForeignAndNullPointersAreIgnoredByDestroyMeshAndDestroyTexture)
{
    const MeshData data = MakeMesh(3, 3, 1);
    const std::vector<std::uint8_t> pixels = Pattern(2, 2, 4);
    IMesh* mesh = R().CreateMesh(data);
    ITexture* texture = R().CreateTexture(pixels.data(), 2, 2, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);

    R().DestroyMesh(nullptr);
    R().DestroyTexture(nullptr);

    FakeMesh fakeMesh;
    FakeTexture fakeTexture;
    R().DestroyMesh(&fakeMesh);
    R().DestroyTexture(&fakeTexture);

    if (VulkanRenderer* second = NewInitializedRenderer())
    {
        IMesh* foreignMesh = second->CreateMesh(data);
        ITexture* foreignTexture = second->CreateTexture(pixels.data(), 2, 2, 4);
        ASSERT_NE(foreignMesh, nullptr);
        ASSERT_NE(foreignTexture, nullptr);
        R().DestroyMesh(foreignMesh);
        R().DestroyTexture(foreignTexture);
        EXPECT_EQ(second->GetLiveMeshCountForTest(), 1u); // the second renderer's resources are untouched
        EXPECT_EQ(second->GetLiveTextureCountForTest(), 1u);
        EXPECT_EQ(second->GetPendingDeletionCountForTest(), 0u);
        MeshData back;
        EXPECT_TRUE(second->ReadMeshForTest(foreignMesh, back));
        EXPECT_FALSE(R().ReadMeshForTest(foreignMesh, back)); // and R() does not know them
        std::vector<std::uint8_t> bytes;
        EXPECT_FALSE(R().ReadTextureForTest(foreignTexture, 0, bytes));
        EXPECT_FALSE(R().GetTextureInfoForTest(foreignTexture).valid);
        EXPECT_TRUE(second->GetTextureInfoForTest(foreignTexture).valid);
    }

    // R()'s own are still there
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 1u);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);

    // A mesh pointer is not a texture and a texture pointer is not a mesh: each Destroy only takes its own kind
    R().DestroyMesh(mesh);
    R().DestroyMesh(mesh); // destroyed twice: the second is a no-op
    R().DestroyTexture(texture);
    R().DestroyTexture(texture);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// Deferred deletion
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, DestroyMeshInsideAnOpenFrameDefersTheFreeUntilThatFramesSlotComesRound)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(4, 6, 1));
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(R().GetLiveAllocationCountForTest(), 2u);

    R().BeginFrame(); // frame 0, slot 0
    R().DestroyMesh(mesh);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 2u);   // retired, not freed: this frame's commands may use them
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
    R().EndFrame();
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);

    R().BeginFrame(); // frame 1, slot 1: does not wait for frame 0, so frees nothing of slot 0's
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 2u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
    R().EndFrame();

    R().BeginFrame(); // frame 2, slot 0 again: frame 0 has finished, its retired buffers are freed
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
    R().EndFrame();
}

TEST_F(VulkanResourceTest, DestroyTextureInsideAnOpenFrameDefersTheFreeToo)
{
    const std::vector<std::uint8_t> pixels = Pattern(8, 8, 4);
    ITexture* texture = R().CreateTexture(pixels.data(), 8, 8, 4);
    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(R().GetLiveAllocationCountForTest(), 1u);

    R().BeginFrame();
    R().DestroyTexture(texture);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 1u);
    R().EndFrame();
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, UpdateMeshAfterASubmittedFrameFreesAtOnceBecauseTheUploadIdledTheQueue)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(3, 3, 1));
    ASSERT_NE(mesh, nullptr);
    Frame(R()); // frame 0 is submitted and may still be running

    // The synchronous upload waits for the queue to go idle, so no frame is in flight and no frame is open: the old
    // buffers are freed immediately
    ASSERT_TRUE(R().UpdateMesh(mesh, MakeMesh(4, 6, 2)));
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
}

TEST_F(VulkanResourceTest, UpdateMeshInsideAnOpenFrameStillDefersEvenThoughTheUploadIdledTheQueue)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(3, 3, 1));
    ASSERT_NE(mesh, nullptr);
    R().BeginFrame(); // the open frame's commands may reference the old buffers
    ASSERT_TRUE(R().UpdateMesh(mesh, MakeMesh(4, 6, 2)));
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 2u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 4u);
    R().EndFrame();
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
}

TEST_F(VulkanResourceTest, ManyUpdatesAndDestroysBeforeAnyFrameDoNotAccumulateRetiredObjects)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(4, 6, 1));
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(R().GetLiveAllocationCountForTest(), 2u);

    for (int i = 0; i < 10000; ++i)
    {
        ASSERT_TRUE(R().UpdateMesh(mesh, MakeMesh(4 + static_cast<std::size_t>(i % 5), 6, i)));
        if (i % 1000 == 0)
        {
            ASSERT_EQ(R().GetPendingDeletionCountForTest(), 0u) << "update " << i;
            ASSERT_EQ(R().GetLiveAllocationCountForTest(), 2u) << "update " << i;
        }
    }
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);

    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    for (int i = 0; i < 200; ++i)
    {
        IMesh* extra = R().CreateMesh(MakeMesh(3, 3, i));
        ITexture* texture = R().CreateTexture(pixels.data(), 4, 4, 4);
        ASSERT_NE(extra, nullptr);
        ASSERT_NE(texture, nullptr);
        R().DestroyMesh(extra);
        R().DestroyTexture(texture);
    }
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
}

TEST_F(VulkanResourceTest, UpdateMeshBeforeAnyFrameIsFreedAtOnce)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(3, 3, 1));
    ASSERT_NE(mesh, nullptr);
    ASSERT_TRUE(R().UpdateMesh(mesh, MakeMesh(3, 3, 2)));
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u);
}

TEST_F(VulkanResourceTest, UpdateMeshEveryFrameForManyMoreFramesThanAreInFlightKeepsTheMeshCorrectAndFreesTheOldBuffers)
{
    IMesh* mesh = R().CreateMesh(MakeMesh(6, 6, 0));
    ASSERT_NE(mesh, nullptr);

    MeshData latest;
    for (int frame = 0; frame < 40; ++frame)
    {
        SCOPED_TRACE(testing::Message() << "frame " << frame);
        latest = MakeMesh(static_cast<std::size_t>(3 + frame % 7), static_cast<std::size_t>(3 + (frame % 5) * 3), frame + 1);
        R().BeginFrame();
        // An update from the middle of the frame, the way text, particles and the GPU cache do it
        ASSERT_TRUE(R().UpdateMesh(mesh, latest));
        ClearTo(R(), {static_cast<int>(frame), 0, 0, 255});
        R().EndFrame();
        R().Present();
        EXPECT_LE(R().GetPendingDeletionCountForTest(), 4u); // at most two frames' worth of retired buffers waits
    }

    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(latest, back));
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 2u); // only the current vertex and index buffers are left
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {39, 0, 0, 255})); // and the frames themselves still rendered
}

TEST_F(VulkanResourceTest, ManyUpdatesAndCreateDestroyCyclesReturnTheLiveAllocationCountToItsBaseline)
{
    IMesh* keeper = R().CreateMesh(MakeMesh(4, 6, 1));
    ASSERT_NE(keeper, nullptr);
    const std::uint32_t baseline = R().GetLiveAllocationCountForTest();
    ASSERT_EQ(baseline, 2u);

    for (int i = 0; i < 1000; ++i)
    {
        R().BeginFrame();
        ASSERT_TRUE(R().UpdateMesh(keeper, MakeMesh(4 + static_cast<std::size_t>(i % 5), 6, i)));
        R().EndFrame();
    }
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), baseline);

    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    for (int i = 0; i < 1000; ++i)
    {
        R().BeginFrame();
        IMesh* mesh = R().CreateMesh(MakeMesh(3, 3, i));
        ITexture* texture = R().CreateTexture(pixels.data(), 4, 4, 4);
        ASSERT_NE(mesh, nullptr);
        ASSERT_NE(texture, nullptr);
        R().DestroyMesh(mesh);
        R().DestroyTexture(texture);
        R().EndFrame();
    }
    Frame(R());
    Frame(R());
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), baseline);
}

TEST_F(VulkanResourceTest, ResourcesCreatedInsideAnOpenFrameWorkAndTheFrameStillRenders)
{
    const std::vector<std::uint8_t> pixels = Pattern(9, 7, 3);
    R().BeginFrame();
    ClearTo(R(), {10, 20, 30, 255});
    IMesh* mesh = R().CreateMesh(MakeMesh(5, 9, 1));
    ITexture* texture = R().CreateTexture(pixels.data(), 9, 7, 3);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(R().UpdateMesh(mesh, MakeMesh(6, 9, 2)));
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {10, 20, 30, 255}));

    std::vector<std::uint8_t> level0;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 0, level0));
    EXPECT_EQ(level0, ExpectedLevel0(pixels, 9, 7, 3));
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(MakeMesh(6, 9, 2), back));
}

TEST_F(VulkanResourceTest, ResizingTheTargetDoesNotDisturbMeshesAndTextures)
{
    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    IMesh* mesh = R().CreateMesh(MakeMesh(4, 6, 1));
    ITexture* texture = R().CreateTexture(pixels.data(), 4, 4, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    R().Resize(32, 16);
    RenderFrame(R());
    ASSERT_TRUE(R().SetRenderTargetSize(20, 10));
    RenderFrame(R());
    MeshData back;
    EXPECT_TRUE(R().ReadMeshForTest(mesh, back));
    std::vector<std::uint8_t> bytes;
    EXPECT_TRUE(R().ReadTextureForTest(texture, 0, bytes));
}

// ---------------------------------------------------------------------------------------------------------------------
// Shutdown and re-initialisation
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(VulkanResourceTest, ResourcesNeverDestroyedAreFreedAtShutdownWithNoValidationError)
{
    const std::vector<std::uint8_t> pixels = Pattern(16, 16, 4);
    IMesh* a = R().CreateMesh(MakeMesh(4, 6, 1));
    IMesh* b = R().CreateMesh(MakeMesh(5, 6, 2));
    IMesh* c = R().CreateMesh(MakeMesh(6, 0, 3));
    ITexture* t1 = R().CreateTexture(pixels.data(), 16, 16, 4);
    ITexture* t2 = R().CreateTexture(pixels.data(), 16, 16, 1);
    ASSERT_TRUE(a && b && c && t1 && t2);
    EXPECT_GT(R().GetLiveAllocationCountForTest(), 0u);

    // Retired but never flushed: one destroyed and one updated inside an open frame, the frame then submitted
    R().BeginFrame();
    R().DestroyMesh(b);
    ASSERT_TRUE(R().UpdateMesh(a, MakeMesh(7, 9, 4)));
    R().DestroyTexture(t2);
    R().EndFrame();
    EXPECT_GT(R().GetPendingDeletionCountForTest(), 0u);

    R().Shutdown(); // frees the queued, then the live
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    const auto stats = R().GetValidationStats();
    EXPECT_EQ(stats.errors, 0u); // a leaked buffer, image, view, sampler or memory block would be an error here
    EXPECT_EQ(stats.warnings, 0u);
}

TEST_F(VulkanResourceTest, DestroyAndUpdateAfterShutdownAreHarmlessAndCreatesFail)
{
    const std::vector<std::uint8_t> pixels = Pattern(4, 4, 4);
    IMesh* mesh = R().CreateMesh(MakeMesh(3, 3, 1));
    ITexture* texture = R().CreateTexture(pixels.data(), 4, 4, 4);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);

    R().Shutdown();
    // The pointers dangle now: they are only compared, never dereferenced
    R().DestroyMesh(mesh);
    R().DestroyTexture(texture);
    EXPECT_FALSE(R().UpdateMesh(mesh, MakeMesh(3, 3, 2)));
    EXPECT_FALSE(R().UpdateMesh(nullptr, MakeMesh(3, 3, 2)));
    EXPECT_EQ(R().CreateMesh(MakeMesh(3, 3, 2)), nullptr);
    EXPECT_EQ(R().CreateTexture(pixels.data(), 4, 4, 4), nullptr);
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(R().ReadTextureForTest(texture, 0, bytes));
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
    R().Shutdown(); // twice
}

TEST_F(VulkanResourceTest, CreateMeshAndCreateTextureOnARendererThatWasNeverInitializedReturnNullptr)
{
    VulkanRenderer& fresh = NewRenderer(); // validation mode set, never initialized
    const std::vector<std::uint8_t> pixels = Pattern(2, 2, 4);
    EXPECT_EQ(fresh.CreateMesh(MakeMesh(3, 3, 1)), nullptr);
    EXPECT_EQ(fresh.CreateTexture(pixels.data(), 2, 2, 4), nullptr);
    EXPECT_FALSE(fresh.UpdateMesh(nullptr, MakeMesh(3, 3, 1)));
    fresh.DestroyMesh(nullptr);
    fresh.DestroyTexture(nullptr);
    EXPECT_EQ(fresh.GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, AfterShutdownTheRendererCanBeInitializedAgainAndCreateResourcesAgain)
{
    const std::vector<std::uint8_t> pixels = Pattern(5, 3, 3);
    IMesh* before = R().CreateMesh(MakeMesh(4, 6, 1));
    ASSERT_NE(before, nullptr);
    R().Shutdown();
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();

    const MeshData data = MakeMesh(5, 6, 2);
    IMesh* mesh = R().CreateMesh(data);
    ITexture* texture = R().CreateTexture(pixels.data(), 5, 3, 3);
    ASSERT_NE(mesh, nullptr);
    ASSERT_NE(texture, nullptr);
    MeshData back;
    ASSERT_TRUE(R().ReadMeshForTest(mesh, back));
    EXPECT_TRUE(SameMesh(data, back));
    std::vector<std::uint8_t> level0;
    ASSERT_TRUE(R().ReadTextureForTest(texture, 0, level0));
    EXPECT_EQ(level0, ExpectedLevel0(pixels, 5, 3, 3));
    RenderFrame(R());
    R().DestroyMesh(mesh);
    R().DestroyTexture(texture);
    RenderFrame(R());
    RenderFrame(R());
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
}

TEST_F(VulkanResourceTest, InitializingAgainWithoutShutdownFreesTheOldResources)
{
    const std::vector<std::uint8_t> pixels = Pattern(5, 3, 4);
    ASSERT_NE(R().CreateMesh(MakeMesh(4, 6, 1)), nullptr);
    ASSERT_NE(R().CreateTexture(pixels.data(), 5, 3, 4), nullptr);
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8)) << R().GetInitMessage();
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u);
    EXPECT_NE(R().CreateMesh(MakeMesh(4, 6, 1)), nullptr);
}
