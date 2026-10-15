#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// The cross-backend diff of the Vulkan tests (#43): a scene rendered on the headless software renderer and on Vulkan is
// compared pixel by pixel, so one scene description checks both backends against each other. The software renderer is the
// reference for unlit, nearest-sampled scenes (exact colours), where the proposal's threshold is |Vulkan - Software| <= 2/255
// per colour channel of every pixel.
//
// Rasterisation tie rules differ between backends (a pixel centre exactly on a triangle edge may fall on either side), so
// the diff can leave out the pixels on a COVERAGE EDGE: a pixel is an edge pixel when its colour differs from a neighbour's
// (8-neighbourhood) in either image, and `edgeBorder` is how many pixels around the edges are left out (1: the edge pixels
// themselves; 2: and the ring around them, ...). `maxFractionOverSecond` lets a small share of the compared pixels differ by
// more than `secondThreshold` (the proposal's rule for lit scenes: at most 1% over 8/255). A comparison that excludes
// nothing and allows nothing over the second threshold is the strict one.
//
// RGB is compared always; alpha only where both images define it meaningfully (compareAlpha: the draws that are opaque).

namespace VulkanTesting::Diff
{
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
        int tolerance = 2;                  // the most a colour channel may differ, in 0..255 units
        int edgeBorder = 0;                 // 0: every pixel counts; n >= 1: pixels within n of a coverage edge do not
        int secondThreshold = 8;            // a looser per-channel limit, for the share below
        double maxFractionOverSecond = 0.0; // the share of compared pixels allowed over `secondThreshold` (not over `tolerance`)
        bool compareAlpha = false;
    };

    struct Result
    {
        int compared = 0;
        int excluded = 0;
        int overTolerance = 0; // compared pixels with a channel over `tolerance`
        int overSecond = 0;    // compared pixels with a channel over `secondThreshold`
        int maxDifference = 0;
        int worstX = -1;
        int worstY = -1;
        bool sizesDiffer = false;

        [[nodiscard]] double FractionOverSecond() const
        {
            return compared > 0 ? static_cast<double>(overSecond) / static_cast<double>(compared) : 0.0;
        }

        [[nodiscard]] std::string Describe() const
        {
            std::ostringstream text;
            text << "compared " << compared << " pixels (" << excluded << " left out at coverage edges), " << overTolerance
                 << " over the tolerance, " << overSecond << " over the second threshold, the largest channel difference "
                 << maxDifference << "/255 at (" << worstX << ", " << worstY << ")";
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

    /// The pixels whose RGB differs from a neighbour's in `a` or in `b`, grown by `border - 1` pixels
    inline std::vector<bool> CoverageEdges(const Image& a, const Image& b, const int border)
    {
        const std::size_t count = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
        std::vector<bool> edges(count, false);
        if (border <= 0)
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
                            if (ChannelDifference(image.Pixel(x, y), image.Pixel(nx, ny), false) > 0)
                            {
                                edges[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)] = true;
                            }
                        }
                    }
                }
            }
        };
        mark(a);
        mark(b);
        for (int grow = 1; grow < border; ++grow)
        {
            std::vector<bool> grown = edges;
            for (int y = 0; y < a.height; ++y)
            {
                for (int x = 0; x < a.width; ++x)
                {
                    if (!edges[static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)])
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
                                grown[static_cast<std::size_t>(ny) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(nx)] = true;
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
        const std::vector<bool> edges = CoverageEdges(a, b, options.edgeBorder);
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                if (edges[static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)])
                {
                    ++result.excluded;
                    continue;
                }
                ++result.compared;
                const int difference = ChannelDifference(a.Pixel(x, y), b.Pixel(x, y), options.compareAlpha);
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
        return result;
    }

    /// Strict (maxFractionOverSecond 0): passes when no compared pixel is over the tolerance. With a share: passes when at
    /// most that share of the compared pixels is over the second threshold
    inline ::testing::AssertionResult Agree(const Image& vulkan, const Image& software, const Options& options)
    {
        const Result result = Compare(vulkan, software, options);
        if (result.sizesDiffer)
        {
            return ::testing::AssertionFailure() << "the images are not the same size";
        }
        if (result.compared == 0)
        {
            return ::testing::AssertionFailure() << "nothing was compared: " << result.Describe();
        }
        // Strict (no share allowed): no pixel over the tolerance. With a share: at most that share over the second threshold
        const bool ok = options.maxFractionOverSecond > 0.0
            ? result.overSecond <= static_cast<int>(options.maxFractionOverSecond * result.compared)
            : result.overTolerance == 0;
        if (ok)
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "Vulkan and the software renderer disagree: " << result.Describe()
                                             << " (tolerance " << options.tolerance << ", edge border " << options.edgeBorder
                                             << ", at most " << options.maxFractionOverSecond << " of the pixels over "
                                             << options.secondThreshold << ")";
    }
}
