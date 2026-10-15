#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "BlendReference.hpp"
#include "CrossBackendDiff.hpp"

// Tests of the GL-semantics blend reference (BlendReference.hpp, #43 stage D) and of the diff's ignore mask. No device is
// needed, so they run on every build that has the Vulkan tests (the Windows legs too). They fix the equation the blended
// Vulkan frames are compared with: out = a * src + (1 - a) * dst (additive: a * src + dst), clamped, rounded, with the
// alpha channel blended by the same factors.

using namespace VulkanTesting;

namespace
{
    constexpr int W = 8;
    constexpr int H = 8;

    Diff::Image Flat(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b, const std::uint8_t a = 255)
    {
        Diff::Image image;
        image.width = W;
        image.height = H;
        image.rgba.resize(static_cast<std::size_t>(W) * H * 4);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
        {
            image.rgba[i] = r;
            image.rgba[i + 1] = g;
            image.rgba[i + 2] = b;
            image.rgba[i + 3] = a;
        }
        return image;
    }

    /// A layer of one colour covering the left half of the image (x < 4)
    Blend::Layer LeftHalf(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b, const double alpha,
                          const bool additive = false)
    {
        Blend::Layer layer;
        layer.colour = Flat(r, g, b);
        layer.covered.assign(static_cast<std::size_t>(W) * H, false);
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W / 2; ++x)
            {
                layer.covered[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)] = true;
            }
        }
        layer.alpha = alpha;
        layer.additive = additive;
        return layer;
    }

    std::array<int, 4> At(const Diff::Image& image, const int x, const int y)
    {
        const std::uint8_t* p = image.Pixel(x, y);
        return {p[0], p[1], p[2], p[3]};
    }
}

TEST(BlendReferenceTest, HalfAlphaRedOverBlackIsHalfRed)
{
    const Diff::Image out = Blend::Composite(Flat(0, 0, 0), LeftHalf(255, 0, 0, 0.5));
    // 0.5 * 255 = 127.5 rounds up; the alpha channel over an opaque target is 0.25 + 0.5 = 0.75 (191.25)
    EXPECT_EQ(At(out, 1, 3), (std::array<int, 4>{128, 0, 0, 191}));
}

TEST(BlendReferenceTest, HalfAlphaRedOverBlueMixesPerChannel)
{
    const Diff::Image out = Blend::Composite(Flat(0, 0, 255), LeftHalf(255, 0, 0, 0.5));
    const auto covered = At(out, 2, 2);
    EXPECT_EQ(covered[0], 128);
    EXPECT_EQ(covered[1], 0);
    EXPECT_EQ(covered[2], 128);
    // The pixels the layer does not cover are the destination
    EXPECT_EQ(At(out, 6, 2), (std::array<int, 4>{0, 0, 255, 255}));
}

TEST(BlendReferenceTest, AdditiveRedOverGreenIsYellow)
{
    const Diff::Image out = Blend::Composite(Flat(0, 255, 0), LeftHalf(255, 0, 0, 1.0, true));
    EXPECT_EQ(At(out, 0, 0), (std::array<int, 4>{255, 255, 0, 255}));
}

TEST(BlendReferenceTest, AdditiveSaturatesAt255)
{
    // Two additive half-alpha draws: 0.5 * 255 = 128 (127.5 rounded), then 128 + 128 = 256 -> 255
    Diff::Image out = Blend::Composite(Flat(0, 0, 0), LeftHalf(255, 0, 0, 0.5, true));
    EXPECT_EQ(At(out, 1, 1)[0], 128);
    out = Blend::Composite(out, LeftHalf(255, 0, 0, 0.5, true));
    EXPECT_EQ(At(out, 1, 1)[0], 255);
    EXPECT_EQ(At(out, 1, 1)[3], 255) << "the alpha channel saturates too";
    // A single additive draw over a bright destination clamps as well
    const Diff::Image bright = Blend::Composite(Flat(200, 200, 200), LeftHalf(255, 255, 255, 1.0, true));
    EXPECT_EQ(At(bright, 0, 0), (std::array<int, 4>{255, 255, 255, 255}));
}

TEST(BlendReferenceTest, AlphaZeroLeavesTheDestinationAndAlphaOneReplacesIt)
{
    const Diff::Image under = Flat(10, 20, 30, 200);
    const Diff::Image none = Blend::Composite(under, LeftHalf(255, 255, 255, 0.0));
    EXPECT_EQ(none.rgba, under.rgba) << "alpha 0 changes nothing, the alpha channel included";
    const Diff::Image noneAdditive = Blend::Composite(under, LeftHalf(255, 255, 255, 0.0, true));
    EXPECT_EQ(noneAdditive.rgba, under.rgba);

    const Diff::Image replaced = Blend::Composite(under, LeftHalf(1, 2, 3, 1.0));
    EXPECT_EQ(At(replaced, 0, 0), (std::array<int, 4>{1, 2, 3, 255})) << "alpha 1: the source, and an opaque target";
    EXPECT_EQ(At(replaced, 7, 0), (std::array<int, 4>{10, 20, 30, 200})) << "uncovered pixels are untouched";
}

TEST(BlendReferenceTest, TheAlphaChannelIsBlendedByTheSameFactors)
{
    // GL: dstA' = srcA * srcA + dstA * (1 - srcA). Over a target of alpha 0: 0.5 * 0.5 = 0.25 -> 64 (63.75)
    const Diff::Image overClear = Blend::Composite(Flat(0, 0, 0, 0), LeftHalf(255, 0, 0, 0.5));
    EXPECT_EQ(At(overClear, 0, 0)[3], 64);
    // Over an opaque target: 0.25 + 1 * 0.5 = 0.75 -> 191 (191.25)
    const Diff::Image overOpaque = Blend::Composite(Flat(0, 0, 0, 255), LeftHalf(255, 0, 0, 0.5));
    EXPECT_EQ(At(overOpaque, 0, 0)[3], 191);
    // Additive: 0.25 + dstA, clamped: over alpha 0 it is 64, over opaque it stays 255
    EXPECT_EQ(At(Blend::Composite(Flat(0, 0, 0, 0), LeftHalf(255, 0, 0, 0.5, true)), 0, 0)[3], 64);
    EXPECT_EQ(At(Blend::Composite(Flat(0, 0, 0, 255), LeftHalf(255, 0, 0, 0.5, true)), 0, 0)[3], 255);
}

TEST(BlendReferenceTest, LayersBlendInSubmissionOrderAndTheOrderMatters)
{
    const Diff::Image under = Flat(0, 0, 0);
    const Blend::Layer red = LeftHalf(255, 0, 0, 0.5);
    const Blend::Layer blue = LeftHalf(0, 0, 255, 0.5);
    const Diff::Image redThenBlue = Blend::Composite(under, std::vector<Blend::Layer>{red, blue});
    const Diff::Image blueThenRed = Blend::Composite(under, std::vector<Blend::Layer>{blue, red});
    // red over black = (128, 0, 0); blue over that = (64, 0, 128)
    EXPECT_EQ(At(redThenBlue, 0, 0)[0], 64);
    EXPECT_EQ(At(redThenBlue, 0, 0)[2], 128);
    EXPECT_EQ(At(blueThenRed, 0, 0)[0], 128);
    EXPECT_EQ(At(blueThenRed, 0, 0)[2], 64);
    EXPECT_NE(redThenBlue.rgba, blueThenRed.rgba);
}

TEST(BlendReferenceTest, ALayerOfTheWrongSizeIsNotApplied)
{
    Blend::Layer layer = LeftHalf(255, 0, 0, 1.0);
    layer.covered.pop_back();
    const Diff::Image under = Flat(1, 2, 3);
    EXPECT_EQ(Blend::Composite(under, layer).rgba, under.rgba);
}

TEST(BlendReferenceTest, CoveredPixelsAreTheOnesThatAreNotTheBackground)
{
    Diff::Image image = Flat(0, 0, 0);
    std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(2) * W + 3) * 4];
    p[1] = 9;
    const std::vector<bool> covered = Blend::CoveredPixels(image, {0, 0, 0});
    int count = 0;
    for (const bool flag : covered)
    {
        count += flag ? 1 : 0;
    }
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(covered[static_cast<std::size_t>(2) * W + 3]);
}

TEST(BlendReferenceTest, DilateGrowsAMaskByTheBorder)
{
    std::vector<bool> mask(static_cast<std::size_t>(W) * H, false);
    mask[static_cast<std::size_t>(4) * W + 4] = true;
    const auto count = [](const std::vector<bool>& m)
    {
        int n = 0;
        for (const bool flag : m)
        {
            n += flag ? 1 : 0;
        }
        return n;
    };
    EXPECT_EQ(count(Blend::Dilate(mask, W, H, 0)), 1);
    EXPECT_EQ(count(Blend::Dilate(mask, W, H, 1)), 9);
    EXPECT_EQ(count(Blend::Dilate(mask, W, H, 2)), 25);
    // At a corner the grown mask is clipped to the image
    std::vector<bool> corner(static_cast<std::size_t>(W) * H, false);
    corner[0] = true;
    EXPECT_EQ(count(Blend::Dilate(corner, W, H, 1)), 4);
}

TEST(BlendReferenceTest, TheDiffIgnoresMaskedPixelsAndCountsThemAsExcluded)
{
    const Diff::Image a = Flat(10, 10, 10);
    Diff::Image b = Flat(10, 10, 10);
    // One pixel is far off
    std::uint8_t* p = &b.rgba[(static_cast<std::size_t>(3) * W + 3) * 4];
    p[0] = 200;

    Diff::Options options;
    EXPECT_FALSE(Diff::Agree(a, b, options)) << "off by 190 and compared";

    std::vector<bool> ignore(static_cast<std::size_t>(W) * H, false);
    ignore[static_cast<std::size_t>(3) * W + 3] = true;
    options.ignore = &ignore;
    EXPECT_TRUE(Diff::Agree(a, b, options)) << "the same pixel left out";
    const Diff::Result result = Diff::Compare(a, b, options);
    EXPECT_EQ(result.excluded, 1);
    EXPECT_EQ(result.compared, W * H - 1);

    // A mask of the wrong size is reported, never taken as 'nothing ignored'
    std::vector<bool> wrong(3, true);
    options.ignore = &wrong;
    EXPECT_TRUE(Diff::Compare(a, b, options).sizesDiffer);
    EXPECT_FALSE(Diff::Agree(a, b, options));
}

TEST(BlendReferenceTest, ABlendedFrameDoesNotAgreeWithAnOpaqueOneOutsideTheMask)
{
    // The reason for the reference: the software frame draws the translucent layer opaque, the composite does not
    const Diff::Image under = Flat(0, 0, 255);
    const Blend::Layer layer = LeftHalf(255, 0, 0, 0.5);
    const Diff::Image composite = Blend::Composite(under, layer);
    Diff::Image opaque = under;
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W / 2; ++x)
        {
            std::uint8_t* p = &opaque.rgba[(static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)) * 4];
            p[0] = 255;
            p[1] = 0;
            p[2] = 0;
        }
    }
    EXPECT_FALSE(Diff::Agree(composite, opaque, Diff::Options{})) << "blended is not opaque";
    // With the layer's pixels ignored they agree: that is the opaque-mask comparison
    std::vector<bool> ignore = Blend::Dilate(layer.covered, W, H, 1);
    Diff::Options options;
    options.ignore = &ignore;
    EXPECT_TRUE(Diff::Agree(composite, opaque, options));
}
