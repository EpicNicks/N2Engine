#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// The cross-backend diff of the Vulkan tests (#43): a scene rendered on the headless software renderer and on Vulkan is
// compared pixel by pixel, so one scene description checks both backends against each other. The software renderer is the
// reference. There are two modes.
//
// STRICT (the default; unlit, nearest-sampled scenes, where the software renderer is exact): no compared pixel may differ
// by more than `tolerance` (2/255) in any colour channel. Nothing is left out unless `edgeBorder` is set (the stage B
// goldens leave nothing out).
//
// LIT (`maxMeanError > 0`; lit and linear-filtered scenes, the proposal's rule): the comparison passes only when ALL of
//   - the MEAN error over the compared pixels is <= maxMeanError (2/255). The error of a pixel is its LARGEST colour channel
//     difference (the stricter reading: a mean over the three channels would be smaller or equal). So pixels that are off by
//     less than the second threshold still count, through the mean: an image where every pixel is off by 7/255 fails;
//   - at most `maxFractionOverSecond` (1%) of the compared pixels are over `secondThreshold` (8/255);
//   - at most `maxExcludedFraction` of the image was left out as coverage edges, so an over-broad exclusion can never pass
//     vacuously; and something was compared.
//
// COVERAGE EDGES. Rasterisation tie rules differ between backends (a pixel centre exactly on a triangle edge may fall on
// either side), so the pixels on the edge of what is drawn are left out: `edgeBorder` 1 leaves out the pixels on both sides
// of an edge (the proposal's 1-pixel border), 2 also the ring around them, and so on.
//   - `coverageEdges` (the LIT definition): a pixel is COVERED in an image when its COVERAGE MASK's colour is not
//     `background` (the clear colour). The mask is a flat unlit render of the same geometry (Options::coverageA/B), so a
//     lit surface that one backend shades black and the other lights is a real difference that is compared and fails, not
//     an 'edge' that is left out. An edge pixel is one whose covered-ness differs from a neighbour's (8-neighbourhood) in either image, or
//     differs between the two images. This depends on coverage only, never on shading, so a smoothly shaded surface is not
//     taken for an edge (the old definition marked every pixel that differed from any neighbour, which in a lit scene is
//     nearly every covered pixel). The one blind spot is a covered pixel that is exactly the background colour (a surface in
//     no light on a black clear): it counts as uncovered, which only widens the exclusion, and the cap below bounds that.
//   - without it (the STRICT definition, unchanged): a pixel is an edge pixel when its RGB differs from any neighbour's.
//
// RGB is compared always; alpha only where both images define it meaningfully (compareAlpha: the draws that are opaque).

namespace VulkanTesting::Diff
{
    /// The measured results of the lit comparisons of this process, one line each (a test that compares calls Record)
    inline std::vector<std::string>& RecordedLines()
    {
        static std::vector<std::string> lines;
        return lines;
    }

    struct Image
    {
        std::vector<std::uint8_t> rgba; // RGBA8, row 0 at the bottom (as ReadFramebuffer returns it)
        int width = 0;
        int height = 0;

        [[nodiscard]] const std::uint8_t* Pixel(const int x, const int y) const
        {
            return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
        }
    };

    struct Options
    {
        int tolerance = 2;                  // strict mode: the most a colour channel may differ, in 0..255 units
        int edgeBorder = 0;                 // 0: every pixel counts; n >= 1: pixels within n of a coverage edge do not
        int secondThreshold = 8;            // lit mode: a looser per-channel limit, for the share below
        double maxFractionOverSecond = 0.0; // lit mode: the share of compared pixels allowed over `secondThreshold`
        double maxMeanError = 0.0;          // > 0 selects the LIT mode: the mean of the per-pixel largest channel difference
        bool coverageEdges = false;         // edges from coverage (pixel != background), not from any colour difference
    // The COVERAGE MASKS of the two images (lit mode): the same geometry, state and camera drawn with a flat unlit colour on
    // each backend, so what is covered is known without any shading. nullptr: the compared image itself serves as its own
    // mask, which is only right where the image is flat-coloured (the harness tests, the unlit-material model golden).
    // Lit scenes MUST pass masks: with the lit image as its own mask a lit object drawn black on one backend would count
    // as 'not covered' there and be left out instead of failing.
    const Image* coverageA = nullptr; // the mask of the first image given to Compare/Agree
    const Image* coverageB = nullptr; // the mask of the second
        std::array<std::uint8_t, 3> background{0, 0, 0}; // the clear colour, for coverageEdges
        double maxExcludedFraction = 1.0;   // the most of the image that may be left out as edges (1: no cap)
        bool compareAlpha = false;
        // Pixels to leave out of the comparison (counted as excluded): one flag per pixel, row 0 at the bottom, the image's
        // size. The blended scenes use it to compare the OPAQUE parts of a frame with the software frame (BlendReference.hpp)
        // and leave out what a blended draw covers, which the software renderer cannot draw. nullptr: nothing is ignored.
        const std::vector<bool>* ignore = nullptr;
    };

    /// The proposal's lit rule: mean error <= 2/255, <= 1% of the compared pixels over 8/255, a 1-pixel border around
    /// each coverage edge (edges from coverage against `background`), at most a quarter of the image left out
    inline Options Lit(const std::array<std::uint8_t, 3>& background = {0, 0, 0})
    {
        Options options;
        options.tolerance = 2;
        options.edgeBorder = 1;
        options.secondThreshold = 8;
        options.maxFractionOverSecond = 0.01;
        options.maxMeanError = 2.0;
        options.coverageEdges = true;
        options.background = background;
        options.maxExcludedFraction = 0.25;
        return options;
    }

    struct Result
    {
        int compared = 0;
        int excluded = 0;
        int totalPixels = 0;
        int overTolerance = 0;  // compared pixels with a channel over `tolerance`
        int overSecond = 0;     // compared pixels with a channel over `secondThreshold`
        double meanError = 0.0; // mean over the compared pixels of the largest channel difference
        int maxDifference = 0;
        int worstX = -1;
        int worstY = -1;
        bool sizesDiffer = false;

        [[nodiscard]] double FractionOverSecond() const
        {
            return compared > 0 ? static_cast<double>(overSecond) / static_cast<double>(compared) : 0.0;
        }

        [[nodiscard]] double ExcludedFraction() const
        {
            return totalPixels > 0 ? static_cast<double>(excluded) / static_cast<double>(totalPixels) : 0.0;
        }

        [[nodiscard]] std::string Describe() const
        {
            std::ostringstream text;
            text << "compared " << compared << " pixels (" << excluded << " left out at coverage edges, "
                 << ExcludedFraction() * 100.0 << "% of the image), mean error " << meanError << "/255, " << overTolerance
                 << " over the tolerance, " << overSecond << " over the second threshold (" << FractionOverSecond() * 100.0
                 << "%), the largest channel difference " << maxDifference << "/255 at (" << worstX << ", " << worstY << ")";
            return text.str();
        }
    };

    inline int ChannelDifference(const std::uint8_t* a, const std::uint8_t* b, const bool alpha)
    {
        int most = 0;
        for (int channel = 0; channel < (alpha ? 4 : 3); ++channel)
        {
            most = std::max(most, std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel])));
        }
        return most;
    }

    namespace Detail
    {
        inline std::size_t Index(const int x, const int y, const int width)
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        }

        inline bool Covered(const Image& image, const int x, const int y, const std::array<std::uint8_t, 3>& background)
        {
            const std::uint8_t* p = image.Pixel(x, y);
            return p[0] != background[0] || p[1] != background[1] || p[2] != background[2];
        }
    }

    /// The pixels on a coverage edge (see the header comment), grown by `options.edgeBorder - 1` pixels. With
    /// `options.coverageA/B` set, coverage is read from those masks, never from the shaded images.
    inline std::vector<bool> CoverageEdges(const Image& imageA, const Image& imageB, const Options& options)
    {
        const Image& a = options.coverageEdges && options.coverageA != nullptr ? *options.coverageA : imageA;
        const Image& b = options.coverageEdges && options.coverageB != nullptr ? *options.coverageB : imageB;
        if (a.width != imageA.width || a.height != imageA.height || b.width != imageA.width || b.height != imageA.height)
        {
            return std::vector<bool>(); // a mask of the wrong size: Compare reports it
        }
        const std::size_t count = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
        std::vector<bool> edges(count, false);
        if (options.edgeBorder <= 0)
        {
            return edges;
        }
        const auto mark = [&](const Image& image)
        {
            for (int y = 0; y < image.height; ++y)
            {
                for (int x = 0; x < image.width; ++x)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx;
                            const int ny = y + dy;
                            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
                            {
                                continue;
                            }
                            const bool edge = options.coverageEdges
                                ? Detail::Covered(image, x, y, options.background) !=
                                    Detail::Covered(image, nx, ny, options.background)
                                : ChannelDifference(image.Pixel(x, y), image.Pixel(nx, ny), false) > 0;
                            if (edge)
                            {
                                edges[Detail::Index(x, y, image.width)] = true;
                            }
                        }
                    }
                }
            }
        };
        mark(a);
        mark(b);
        if (options.coverageEdges)
        {
            // A pixel covered in one image and not in the other is a tie-rule difference whatever its neighbours say
            for (int y = 0; y < a.height; ++y)
            {
                for (int x = 0; x < a.width; ++x)
                {
                    if (Detail::Covered(a, x, y, options.background) != Detail::Covered(b, x, y, options.background))
                    {
                        edges[Detail::Index(x, y, a.width)] = true;
                    }
                }
            }
        }
        for (int grow = 1; grow < options.edgeBorder; ++grow)
        {
            std::vector<bool> grown = edges;
            for (int y = 0; y < a.height; ++y)
            {
                for (int x = 0; x < a.width; ++x)
                {
                    if (!edges[Detail::Index(x, y, a.width)])
                    {
                        continue;
                    }
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx;
                            const int ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < a.width && ny < a.height)
                            {
                                grown[Detail::Index(nx, ny, a.width)] = true;
                            }
                        }
                    }
                }
            }
            edges = std::move(grown);
        }
        return edges;
    }

    inline Result Compare(const Image& a, const Image& b, const Options& options)
    {
        Result result;
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
        {
            result.sizesDiffer = true;
            return result;
        }
        result.totalPixels = a.width * a.height;
        const std::vector<bool> edges = CoverageEdges(a, b, options);
        if (edges.size() != static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height))
        {
            result.sizesDiffer = true; // a coverage mask that is not the image's size
            return result;
        }
        if (options.ignore != nullptr && options.ignore->size() != edges.size())
        {
            result.sizesDiffer = true; // an ignore mask that is not the image's size
            return result;
        }
        long long errorSum = 0;
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                if (edges[Detail::Index(x, y, a.width)] ||
                    (options.ignore != nullptr && (*options.ignore)[Detail::Index(x, y, a.width)]))
                {
                    ++result.excluded;
                    continue;
                }
                ++result.compared;
                const int difference = ChannelDifference(a.Pixel(x, y), b.Pixel(x, y), options.compareAlpha);
                errorSum += difference;
                if (difference > result.maxDifference)
                {
                    result.maxDifference = difference;
                    result.worstX = x;
                    result.worstY = y;
                }
                result.overTolerance += difference > options.tolerance ? 1 : 0;
                result.overSecond += difference > options.secondThreshold ? 1 : 0;
            }
        }
        result.meanError = result.compared > 0 ? static_cast<double>(errorSum) / static_cast<double>(result.compared) : 0.0;
        return result;
    }

    /// Strict (maxMeanError 0): passes when no compared pixel is over the tolerance. Lit (maxMeanError > 0): passes when the
    /// mean error, the share over the second threshold and the excluded share are all within their limits (see the header)
    inline ::testing::AssertionResult Agree(const Image& vulkan, const Image& software, const Options& options)
    {
        const Result result = Compare(vulkan, software, options);
        if (result.sizesDiffer)
        {
            return ::testing::AssertionFailure() << "the images (or a coverage mask) are not the same size";
        }
        if (options.maxMeanError <= 0.0 && options.maxFractionOverSecond > 0.0)
        {
            return ::testing::AssertionFailure()
                << "a share over the second threshold is only allowed together with a mean error limit (the lit rule)";
        }
        if (result.compared == 0)
        {
            return ::testing::AssertionFailure() << "nothing was compared: " << result.Describe();
        }
        if (result.ExcludedFraction() > options.maxExcludedFraction)
        {
            return ::testing::AssertionFailure()
                << "too much of the image was left out as coverage edges (more than " << options.maxExcludedFraction * 100.0
                << "%), so the comparison would be vacuous: " << result.Describe();
        }
        bool ok = false;
        if (options.maxMeanError > 0.0)
        {
            ok = result.meanError <= options.maxMeanError &&
                static_cast<double>(result.overSecond) <= options.maxFractionOverSecond * static_cast<double>(result.compared);
        }
        else
        {
            ok = result.overTolerance == 0;
        }
        if (ok)
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "Vulkan and the software renderer disagree: " << result.Describe()
                                             << " (tolerance " << options.tolerance << ", mean error limit "
                                             << options.maxMeanError << ", edge border " << options.edgeBorder
                                             << ", at most " << options.maxFractionOverSecond * 100.0 << "% of the pixels over "
                                             << options.secondThreshold << ")";
    }
}
