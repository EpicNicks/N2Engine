#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "common/CrossBackendDiff.hpp"
#include "VulkanDrawTestUtil.hpp"

// What the Vulkan text tests share (#43, stage E): VulkanTextShaderTests.cpp, VulkanTextGoldenTests.cpp,
// VulkanTextEffectsGoldenTests.cpp and VulkanTextPassesGoldenTests.cpp.
//
// The Vulkan text shader is a port of OpenGL's: antialiased over about one pixel and BLENDED, so a glyph edge pixel is a blend
// of the text colour and what is behind it, where the software renderer alpha-tests (a pixel is the text colour or untouched).
// The helpers here read an antialiased frame the way the software tests read theirs:
//   - the BLEND WEIGHT of a pixel: how far it lies from the background towards the ink colour (the projection onto the line
//     between the two). 1 is pure ink, 0 pure background; an edge pixel is in between;
//   - the THRESHOLDED frame: ink where the weight is at least one half, the background elsewhere. The coverage ramp is
//     centred on the glyph's 0.5 iso-line, which is where the software renderer's alpha test cuts, so the thresholded frame is
//     what the software renderer draws (to within a pixel of rounding), and the software tests' structural checks (bounding
//     boxes, column runs, counts) apply to it unchanged;
//   - the AREA of the ink: the sum of the weights, which a glyph that is too thin or too fat changes.
// And the cross-backend comparison of antialiased text (CompareAntialiased): the coverage-based edge exclusion of
// CrossBackendDiff.hpp with the STRICT tolerance on the pixels that remain, a cap on the excluded share so the comparison is
// never vacuous, and the area check against the software ink count.

namespace VulkanTesting::Ink
{
    using Draw::Frame;
    using Draw::Rgb;

    inline constexpr Rgb kDark{0, 0, 0};

    // ---- The synthetic SDF the renderer-level tests draw (the software renderer's text shader tests use the same one) -------
    // A disc of radius DiscRadius texels in the middle of a DiscSize-texel square atlas, edge at 128 (0.5), 16 levels per
    // texel. Drawn over a quad that spans NDC -0.5..0.5 on a 64x64 target, one texel covers one pixel.
    inline constexpr int DiscSize = 32;
    inline constexpr float DiscRadius = 10.0f;
    inline constexpr float DiscCentrePx = 32.0f;
    inline constexpr float DiscRadiusPx = DiscRadius * (Draw::Size * 0.5f) / DiscSize;

    inline std::vector<std::uint8_t> DiscSdf()
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(DiscSize) * DiscSize);
        constexpr float centre = DiscSize * 0.5f;
        for (int y = 0; y < DiscSize; ++y)
        {
            for (int x = 0; x < DiscSize; ++x)
            {
                const float dist = std::hypot(static_cast<float>(x) + 0.5f - centre, static_cast<float>(y) + 0.5f - centre);
                const float value = std::clamp(128.0f + (DiscRadius - dist) * 16.0f, 0.0f, 255.0f);
                pixels[static_cast<std::size_t>(y) * DiscSize + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(value);
            }
        }
        return pixels;
    }

    /// The distance of a pixel's centre from the disc's centre, in pixels
    inline float DistanceFromDiscCentre(const int x, const int y)
    {
        return std::hypot(static_cast<float>(x) + 0.5f - DiscCentrePx, static_cast<float>(y) + 0.5f - DiscCentrePx);
    }

    // ---- Reading an antialiased frame -------------------------------------------------------------------------------------

    /// How far `pixel` lies from `background` towards `ink` (0 background, 1 ink), by projection onto the line between them.
    /// Not clamped: a pixel that is not on the line (a mix of three colours) gives the part of it that is along the line
    inline double Weight(const Rgb& pixel, const Rgb& ink, const Rgb& background)
    {
        const double dr = static_cast<double>(ink.r) - background.r;
        const double dg = static_cast<double>(ink.g) - background.g;
        const double db = static_cast<double>(ink.b) - background.b;
        const double denominator = dr * dr + dg * dg + db * db;
        if (denominator <= 0.0)
        {
            return 0.0;
        }
        const double numerator = (static_cast<double>(pixel.r) - background.r) * dr +
            (static_cast<double>(pixel.g) - background.g) * dg + (static_cast<double>(pixel.b) - background.b) * db;
        return numerator / denominator;
    }

    /// At least half ink: the pixels the software renderer's alpha test (at the same 0.5 iso-line) would draw
    inline bool IsInk(const Rgb& pixel, const Rgb& ink, const Rgb& background)
    {
        return Weight(pixel, ink, background) >= 0.5;
    }

    /// `frame` with every pixel replaced by `ink` (weight >= 0.5) or `background`: what the software renderer draws
    inline Frame Thresholded(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        Frame out = frame;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb value = IsInk(frame.At(x, y), ink, background) ? ink : background;
                const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 4;
                out.rgba[i] = value.r;
                out.rgba[i + 1] = value.g;
                out.rgba[i + 2] = value.b;
                out.rgba[i + 3] = 255;
            }
        }
        return out;
    }

    /// The pixels that are exactly the ink or exactly the background are not edge pixels: this counts the others
    inline int InBetween(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        int count = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                count += (pixel == ink || pixel == background) ? 0 : 1;
            }
        }
        return count;
    }

    /// The pixels with a weight of at least one half
    inline int InkPixels(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        int count = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                count += IsInk(frame.At(x, y), ink, background) ? 1 : 0;
            }
        }
        return count;
    }

    /// Pixels that are not on the straight line from `background` to `ink` (the blend of the text colour with the
    /// background, however antialiased): a hue the text colour cannot make. Two channel steps of tolerance for rounding
    inline int OffTheLine(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        int off = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                const double w = Weight(pixel, ink, background);
                const double r = background.r + w * (static_cast<double>(ink.r) - background.r);
                const double g = background.g + w * (static_cast<double>(ink.g) - background.g);
                const double b = background.b + w * (static_cast<double>(ink.b) - background.b);
                if (w < -0.01 || w > 1.01 || std::abs(r - pixel.r) > 2.0 || std::abs(g - pixel.g) > 2.0 ||
                    std::abs(b - pixel.b) > 2.0)
                {
                    ++off;
                }
            }
        }
        return off;
    }

    /// The ink's area in pixels: the sum of the weights (each clamped to 0..1)
    inline double InkArea(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        double area = 0.0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                area += std::clamp(Weight(frame.At(x, y), ink, background), 0.0, 1.0);
            }
        }
        return area;
    }

    /// The bounding box of the ink pixels (weight >= 0.5) and how many there are
    inline Draw::Coverage InkCoverage(const Frame& frame, const Rgb& ink, const Rgb& background)
    {
        Draw::Coverage c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (IsInk(frame.At(x, y), ink, background))
                {
                    ++c.count;
                    c.minX = std::min(c.minX, x);
                    c.maxX = std::max(c.maxX, x);
                    c.minY = std::min(c.minY, y);
                    c.maxY = std::max(c.maxY, y);
                }
            }
        }
        return c;
    }

    /// `frame` with every pixel replaced by the nearest colour of `palette` (squared RGB distance): a frame of antialiased
    /// layers (a face, an outline, a shadow, the background) read as the layer each pixel is mostly. A pixel halfway between
    /// two layers goes to either; the software tests' exact-colour sets are the same sets to within those edge pixels
    inline Frame Nearest(const Frame& frame, const std::vector<Rgb>& palette)
    {
        Frame out = frame;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                Rgb best = palette.front();
                long bestDistance = -1;
                for (const Rgb& candidate : palette)
                {
                    const long dr = static_cast<long>(pixel.r) - candidate.r;
                    const long dg = static_cast<long>(pixel.g) - candidate.g;
                    const long db = static_cast<long>(pixel.b) - candidate.b;
                    const long distance = dr * dr + dg * dg + db * db;
                    if (bestDistance < 0 || distance < bestDistance)
                    {
                        bestDistance = distance;
                        best = candidate;
                    }
                }
                const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 4;
                out.rgba[i] = best.r;
                out.rgba[i + 1] = best.g;
                out.rgba[i + 2] = best.b;
                out.rgba[i + 3] = 255;
            }
        }
        return out;
    }

    /// The largest difference of any colour channel (alpha not compared) between two frames of one size, and how many pixels
    /// differ by more than `tolerance`
    struct FrameDifference
    {
        int maxChannel = 0;
        int overTolerance = 0;
        bool sizesDiffer = false;
    };

    inline FrameDifference Difference(const Frame& a, const Frame& b, const int tolerance)
    {
        FrameDifference result;
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
        {
            result.sizesDiffer = true;
            return result;
        }
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                const Rgb pa = a.At(x, y);
                const Rgb pb = b.At(x, y);
                const int d = std::max({std::abs(static_cast<int>(pa.r) - pb.r), std::abs(static_cast<int>(pa.g) - pb.g),
                                        std::abs(static_cast<int>(pa.b) - pb.b)});
                result.maxChannel = std::max(result.maxChannel, d);
                result.overTolerance += d > tolerance ? 1 : 0;
            }
        }
        return result;
    }

    inline Diff::Image AsImage(const Frame& frame)
    {
        return Diff::Image{frame.rgba, frame.width, frame.height};
    }

    // ---- The cross-backend comparison of antialiased text --------------------------------------------------------------

    /// How an antialiased Vulkan frame is compared with the software frame of the same scene
    struct AntialiasedRule
    {
        /// 1: the pixels on both sides of every coverage edge are left out (the ramp is about one pixel wide); 2 where
        /// softness or minification widens the ramp. Stated for each scene where it is used
        int edgeBorder = 1;
        /// The most of the image that may be left out as coverage edges (the comparison is never vacuous)
        double maxExcludedFraction = 0.5;
        /// The summed Vulkan ink coverage must be within this share of the software ink pixel count
        double maxAreaError = 0.05;
        /// true: a pixel is an edge pixel when its coverage (not the background colour) differs from a neighbour's or between
        /// the two frames: one ink colour over the background. false: when its colour differs from a neighbour's in either
        /// frame (the strict definition of CrossBackendDiff.hpp): frames of several layers (a face, an outline, a shadow),
        /// where a boundary between two layers is an edge too
        bool coverageEdges = true;
        /// false: no area check (a frame of several layers has no single ink colour; the caller compares the layers' pixel
        /// counts after classifying the frames)
        bool checkArea = true;
        /// The least number of software ink pixels (any pixel that is not the background, when checkArea is off) that must be
        /// COMPARED, i.e. not left out as edge pixels: the glyph interiors, so the strict comparison is not just of empty
        /// background
        int minInteriorPixels = 50;
    };

    /// The measured numbers of one comparison, for the PR body and the test log
    struct AntialiasedNumbers
    {
        Diff::Result diff;
        double vulkanArea = 0.0;
        int softwareInk = 0;
        double areaRatio = 0.0;
        int interiorCompared = 0;
    };

    /// Compares `vulkan` (antialiased, blended) with `software` (alpha-tested) for a scene of `ink` text over `background`
    /// (both flat colours, so each frame serves as its own coverage mask: a pixel is covered when it is not the
    /// background):
    ///   - STRICT (2/255 per channel, never loosened) on every pixel that is not within `edgeBorder` of a coverage edge of
    ///     either frame, which is the glyph interiors and the background;
    ///   - at most `maxExcludedFraction` of the image left out;
    ///   - the AREA: the sum of the Vulkan blend weights against the software ink pixel count, within `maxAreaError`.
    /// The numbers are printed (and recorded) under `label`.
    inline ::testing::AssertionResult CompareAntialiased(const std::string& label, const Frame& vulkan, const Frame& software,
                                                         const Rgb& ink, const Rgb& background, const AntialiasedRule& rule,
                                                         AntialiasedNumbers* numbers = nullptr)
    {
        Diff::Options options;
        options.coverageEdges = rule.coverageEdges;
        options.edgeBorder = rule.edgeBorder;
        options.background = {background.r, background.g, background.b};
        options.maxExcludedFraction = rule.maxExcludedFraction;
        const Diff::Image vulkanImage = AsImage(vulkan);
        const Diff::Image softwareImage = AsImage(software);

        AntialiasedNumbers measured;
        measured.diff = Diff::Compare(vulkanImage, softwareImage, options);
        measured.vulkanArea = InkArea(vulkan, ink, background);
        measured.softwareInk = InkPixels(software, ink, background);
        measured.areaRatio = measured.softwareInk > 0 ? measured.vulkanArea / static_cast<double>(measured.softwareInk) : 0.0;
        // The software pixels that are inside a glyph (not the background) and were COMPARED: the interior of the ink
        {
            const std::vector<bool> edges = Diff::CoverageEdges(vulkanImage, softwareImage, options);
            for (int y = 0; y < software.height && edges.size() == static_cast<std::size_t>(software.width) * software.height; ++y)
            {
                for (int x = 0; x < software.width; ++x)
                {
                    const bool edge = edges[static_cast<std::size_t>(y) * static_cast<std::size_t>(software.width) + static_cast<std::size_t>(x)];
                    measured.interiorCompared += (!edge && !(software.At(x, y) == background)) ? 1 : 0;
                }
            }
        }
        if (numbers != nullptr)
        {
            *numbers = measured;
        }
        const std::string area = rule.checkArea
            ? "; ink area " + std::to_string(measured.vulkanArea) + " px against the software's " +
                std::to_string(measured.softwareInk) + " (ratio " + std::to_string(measured.areaRatio) + ")"
            : std::string("; no single ink colour here, the layers' areas are compared by the caller");
        const std::string described = measured.diff.Describe() + area + ", edge border " + std::to_string(rule.edgeBorder) + ", " +
            std::to_string(measured.interiorCompared) + " ink pixels compared away from the edges";
        std::cout << "[ cross-diff ] " << label << " (antialiased, strict): " << described << std::endl;
        Diff::RecordedLines().push_back(label + " (antialiased, strict): " + described);

        const ::testing::AssertionResult agree = Diff::Agree(vulkanImage, softwareImage, options);
        if (!agree)
        {
            return ::testing::AssertionFailure() << label << ": " << agree.message();
        }
        if (measured.interiorCompared < rule.minInteriorPixels)
        {
            return ::testing::AssertionFailure()
                << label << ": only " << measured.interiorCompared << " ink pixels were compared away from the edges (at least "
                << rule.minInteriorPixels << " are needed), so the strict comparison would be nearly vacuous: " << described;
        }
        if (!rule.checkArea)
        {
            return ::testing::AssertionSuccess();
        }
        if (measured.softwareInk <= 0)
        {
            return ::testing::AssertionFailure() << label << ": the software frame has no ink, so the area check is vacuous";
        }
        if (std::abs(measured.areaRatio - 1.0) > rule.maxAreaError)
        {
            return ::testing::AssertionFailure()
                << label << ": the Vulkan ink area is " << measured.vulkanArea << " px, the software ink is "
                << measured.softwareInk << " px (ratio " << measured.areaRatio << ", allowed " << 1.0 - rule.maxAreaError
                << " to " << 1.0 + rule.maxAreaError << "): a glyph that is too thin or too fat";
        }
        return ::testing::AssertionSuccess();
    }
}
