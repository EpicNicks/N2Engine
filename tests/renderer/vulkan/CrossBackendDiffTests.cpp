#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/CrossBackendDiff.hpp"

// Tests of the cross-backend diff harness itself (#43, stage C). No device is needed, so they run on every build that
// has the Vulkan tests (the Windows legs too). The lit criterion is the proposal's: mean error <= 2/255 AND <= 1% of the
// compared pixels over 8/255, after a 1-pixel border around each coverage edge; the strict mode (unlit goldens) is
// unchanged.

using namespace VulkanTesting::Diff;

namespace
{
    constexpr int W = 64;
    constexpr int H = 64;

    /// A W x H image of one colour (alpha 255)
    Image Flat(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
    {
        Image image;
        image.width = W;
        image.height = H;
        image.rgba.resize(static_cast<std::size_t>(W) * H * 4);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
        {
            image.rgba[i] = r;
            image.rgba[i + 1] = g;
            image.rgba[i + 2] = b;
            image.rgba[i + 3] = 255;
        }
        return image;
    }

    /// Adds `delta` to every channel of the pixel (clamped to 0..255)
    void Shift(Image& image, const int x, const int y, const int delta)
    {
        std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
        for (int channel = 0; channel < 3; ++channel)
        {
            p[channel] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(p[channel]) + delta, 0, 255));
        }
    }

    void Set(Image& image, const int x, const int y, const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
    {
        std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
    }

    /// The first `count` pixels in a spread-out order (every pixel of the image is visited once over the whole range)
    std::vector<std::pair<int, int>> SpreadPixels(const int count)
    {
        std::vector<std::pair<int, int>> pixels;
        constexpr int total = W * H;
        constexpr int step = 97; // coprime with 4096
        int index = 0;
        for (int i = 0; i < count; ++i)
        {
            pixels.emplace_back(index % W, index / W);
            index = (index + step) % total;
        }
        return pixels;
    }

    /// A smooth gradient on a non-background colour (every pixel is "covered"), left to right and bottom to top
    Image Gradient()
    {
        Image image = Flat(0, 0, 0);
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                Set(image, x, y, static_cast<std::uint8_t>(40 + x * 3), static_cast<std::uint8_t>(40 + y * 3),
                    static_cast<std::uint8_t>(120 + (x + y)));
            }
        }
        return image;
    }
}

TEST(CrossBackendDiffTest, EveryPixelOffBySevenFailsTheLitCriterionEvenThoughNoPixelIsOverEight)
{
    const Image a = Flat(100, 100, 100);
    Image b = a;
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            Shift(b, x, y, 7);
        }
    }
    const Result result = Compare(a, b, Lit());
    EXPECT_EQ(result.overSecond, 0) << "no pixel is over 8/255";
    EXPECT_DOUBLE_EQ(result.meanError, 7.0);
    EXPECT_FALSE(Agree(a, b, Lit())) << "the mean error 7/255 is over 2/255: pixels between the two thresholds count";
}

TEST(CrossBackendDiffTest, EveryPixelOffByOneOrTwoPassesTheLitCriterion)
{
    const Image a = Flat(100, 100, 100);
    Image b = a;
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            Shift(b, x, y, (x + y) % 2 == 0 ? 1 : 2);
        }
    }
    const Result result = Compare(a, b, Lit());
    EXPECT_NEAR(result.meanError, 1.5, 1e-9);
    EXPECT_TRUE(Agree(a, b, Lit())) << result.Describe();
}

TEST(CrossBackendDiffTest, AllPixelsOffByThreeFailTheLitCriterion)
{
    const Image a = Flat(100, 100, 100);
    Image b = a;
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            Shift(b, x, y, 3);
        }
    }
    EXPECT_FALSE(Agree(a, b, Lit())) << "a mean error of 3/255 is over the limit";
}

TEST(CrossBackendDiffTest, AHalfPercentOutlierShareOffByFiftyPassesAndTwoPercentFails)
{
    const Image a = Flat(100, 100, 100);

    Image half = a;
    for (const auto& [x, y] : SpreadPixels(20)) // 20 of 4096 = 0.49%
    {
        Shift(half, x, y, 50);
    }
    const Result halfResult = Compare(a, half, Lit());
    EXPECT_EQ(halfResult.overSecond, 20);
    EXPECT_LT(halfResult.meanError, 2.0);
    EXPECT_TRUE(Agree(a, half, Lit())) << halfResult.Describe();

    Image two = a;
    for (const auto& [x, y] : SpreadPixels(82)) // 82 of 4096 = 2.0%
    {
        Shift(two, x, y, 50);
    }
    const Result twoResult = Compare(a, two, Lit());
    EXPECT_EQ(twoResult.overSecond, 82);
    EXPECT_LT(twoResult.meanError, 2.0) << "the mean is fine: it is the share over 8/255 that fails";
    EXPECT_FALSE(Agree(a, two, Lit()));
}

TEST(CrossBackendDiffTest, TheStrictModeFailsOnePixelOffByThreeAndPassesOneOffByTwo)
{
    const Image a = Flat(100, 100, 100);

    Image three = a;
    Shift(three, 10, 10, 3);
    EXPECT_FALSE(Agree(a, three, Options{}));

    Image two = a;
    Shift(two, 10, 10, 2);
    EXPECT_TRUE(Agree(a, two, Options{}));

    EXPECT_TRUE(Agree(a, a, Options{}));
}

TEST(CrossBackendDiffTest, ASharePastTheToleranceIsOnlyAllowedWithAMeanLimit)
{
    // The old "share" mode had no mean criterion and pixels between the two thresholds could never fail it: it is refused
    const Image a = Flat(100, 100, 100);
    Options options;
    options.maxFractionOverSecond = 0.01;
    EXPECT_FALSE(Agree(a, a, options));
}

TEST(CrossBackendDiffTest, ASmoothGradientIsNotTakenForAnEdge)
{
    const Image gradient = Gradient();
    const Result lit = Compare(gradient, gradient, Lit());
    EXPECT_EQ(lit.excluded, 0) << "coverage edges come from coverage, not from shading: a covered gradient has none";
    EXPECT_EQ(lit.compared, W * H);
    EXPECT_TRUE(Agree(gradient, gradient, Lit()));

    // What the definition used to do: every pixel of the gradient differs from a neighbour, so all of it is left out
    Options old;
    old.edgeBorder = 1;
    const Result before = Compare(gradient, gradient, old);
    EXPECT_GT(before.ExcludedFraction(), 0.9) << "the colour-difference definition leaves out nearly the whole image";
}

TEST(CrossBackendDiffTest, ASilhouetteMovedByAPixelIsLeftOutAndTheRestIsCompared)
{
    // A bright square on a black background; in the second image it is one pixel to the right (a tie-rule difference)
    Image a = Flat(0, 0, 0);
    Image b = Flat(0, 0, 0);
    for (int y = 10; y < 50; ++y)
    {
        for (int x = 20; x < 40; ++x)
        {
            Set(a, x, y, 200, 180, 160);
            Set(b, x + 1, y, 200, 180, 160);
        }
    }
    const Result result = Compare(a, b, Lit());
    EXPECT_GT(result.excluded, 0);
    EXPECT_LT(result.ExcludedFraction(), 0.25);
    EXPECT_EQ(result.maxDifference, 0) << "every pixel that differs is on a coverage edge and is left out";
    EXPECT_TRUE(Agree(a, b, Lit())) << result.Describe();

    // Without the border the moved silhouette is a real difference
    Options noBorder = Lit();
    noBorder.edgeBorder = 0;
    EXPECT_FALSE(Agree(a, b, noBorder));
}

TEST(CrossBackendDiffTest, TheExclusionCapTripsWhenTooMuchIsLeftOut)
{
    // A checkerboard of covered and background pixels: every pixel is on a coverage edge
    Image board = Flat(0, 0, 0);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            if ((x + y) % 2 == 0)
            {
                Set(board, x, y, 200, 200, 200);
            }
        }
    }
    const Result result = Compare(board, board, Lit());
    EXPECT_GT(result.ExcludedFraction(), 0.9);
    EXPECT_FALSE(Agree(board, board, Lit())) << "identical images, but the exclusion covers the image: vacuous, so it fails";

    // A looser cap lets the same pair through only while something is still compared
    Options uncapped = Lit();
    uncapped.maxExcludedFraction = 1.0;
    EXPECT_FALSE(Agree(board, board, uncapped)) << "nothing is compared when everything is excluded";
}

TEST(CrossBackendDiffTest, TheCapAlsoStopsALargeButNotTotalExclusion)
{
    // Vertical stripes four pixels wide: the outer two pixels of every stripe are edge pixels, so about half of the image
    Image stripes = Flat(0, 0, 0);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            if ((x / 4) % 2 == 0)
            {
                Set(stripes, x, y, 200, 200, 200);
            }
        }
    }
    const Result result = Compare(stripes, stripes, Lit());
    EXPECT_GT(result.ExcludedFraction(), 0.25);
    EXPECT_LT(result.ExcludedFraction(), 1.0);
    EXPECT_GT(result.compared, 0);
    EXPECT_FALSE(Agree(stripes, stripes, Lit())) << "over the 25% cap";
}

TEST(CrossBackendDiffTest, ASizeMismatchFails)
{
    Image a = Flat(1, 2, 3);
    Image b = a;
    b.width = W / 2;
    b.rgba.resize(static_cast<std::size_t>(b.width) * H * 4);
    EXPECT_FALSE(Agree(a, b, Options{}));
    EXPECT_FALSE(Agree(a, b, Lit()));
}

TEST(CrossBackendDiffTest, NothingComparedFails)
{
    Image empty;
    EXPECT_FALSE(Agree(empty, empty, Options{}));
    EXPECT_FALSE(Agree(empty, empty, Lit()));
}

TEST(CrossBackendDiffTest, ResultDescribesTheMeanError)
{
    const Image a = Flat(100, 100, 100);
    Image b = a;
    Shift(b, 3, 3, 40);
    const Result result = Compare(a, b, Lit());
    EXPECT_NE(result.Describe().find("mean error"), std::string::npos);
    EXPECT_EQ(result.maxDifference, 40);
    EXPECT_EQ(result.worstX, 3);
    EXPECT_EQ(result.worstY, 3);
}

TEST(CrossBackendDiffTest, ALitObjectDrawnBlackOnOneBackendFailsInsteadOfBeingLeftOutAsAnEdge)
{
    // A 16 x 16 lit object on a black clear. The coverage mask (flat unlit geometry) is the same on both backends; the
    // second image shades the whole object black. Judged by its own pixels that object would count as 'not covered' there
    // and be left out; judged by the mask it is a real difference
    Image lit = Flat(0, 0, 0);
    Image mask = Flat(0, 0, 0);
    for (int y = 20; y < 36; ++y)
    {
        for (int x = 20; x < 36; ++x)
        {
            Set(lit, x, y, 150, 140, 130);
            Set(mask, x, y, 255, 255, 255);
        }
    }
    const Image black = Flat(0, 0, 0);

    Options options = Lit();
    options.coverageA = &mask;
    options.coverageB = &mask;
    const Result result = Compare(lit, black, options);
    EXPECT_GT(result.maxDifference, 100) << "the interior of the object is compared";
    EXPECT_GT(result.overSecond, 100);
    EXPECT_FALSE(Agree(lit, black, options)) << result.Describe();

    // The same pair with the images as their own masks: the vacuous pass the masks exist to prevent
    Options byImages = Lit();
    EXPECT_GT(Compare(lit, black, byImages).excluded, result.excluded) << "the old rule leaves the object out";

    // A genuine tie-rule difference (the masks themselves differ by a pixel) is still left out
    Image shifted = mask;
    for (int y = 20; y < 36; ++y)
    {
        Set(shifted, 20, y, 0, 0, 0);
        Set(shifted, 36, y, 255, 255, 255);
    }
    Image litShifted = lit;
    for (int y = 20; y < 36; ++y)
    {
        Set(litShifted, 20, y, 0, 0, 0);
        Set(litShifted, 36, y, 150, 140, 130);
    }
    Options tie = Lit();
    tie.coverageA = &mask;
    tie.coverageB = &shifted;
    EXPECT_TRUE(Agree(lit, litShifted, tie)) << Compare(lit, litShifted, tie).Describe();
}

TEST(CrossBackendDiffTest, ACoverageMaskOfTheWrongSizeFails)
{
    const Image a = Flat(1, 2, 3);
    Image small = Flat(0, 0, 0);
    small.width = W / 2;
    small.rgba.resize(static_cast<std::size_t>(small.width) * H * 4);
    Options options = Lit();
    options.coverageA = &small;
    options.coverageB = &a;
    EXPECT_FALSE(Agree(a, a, options));
}
