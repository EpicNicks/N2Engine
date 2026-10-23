#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/common/GpuShaderInterface.hpp>

#include "common/BlendReference.hpp"
#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"
#include "VulkanTextTestUtil.hpp"

// The Vulkan text shader at renderer level (#43, stage E): the seven SoftwareTextShaderTest cases of
// tests/renderer/software/SoftwareRendererGoldenTests.cpp with the same SDF disc atlas, quads and frame size, but with the
// expectations of OpenGL's text shader, which the Vulkan one is a port of, then what only a backend that antialiases and
// blends can show. Real frames on a Vulkan device (Mesa lavapipe in CI; no window, the offscreen target) with validation on.
//
// Where the software renderer and Vulkan differ, and the tests say so at each assertion:
//   - the SOFTWARE text shader ALPHA-TESTS at 0.5: a pixel is the text colour or untouched, nothing in between, and it
//     writes depth wherever the draw depth-tests, even when the draw's depthWrite is off. VULKAN (and OpenGL) ramp the
//     coverage over about one screen pixel around the same 0.5 iso-line and BLEND, so an edge pixel is in between, and it
//     writes depth only as the draw's state says: the Transparent queue's state writes none, so an opaque quad drawn later
//     BEHIND the text covers the text's whole quad;
//   - what the two agree on: the interior of a glyph (exactly the text colour), the background, the colour (albedo times
//     vertex colour), and that a fragment below alpha 0.01 draws nothing, colour or depth.
//
// The software frame of the same disc scene is rendered here too (it needs no device), for the cross-backend check: the
// >= 50% disc is the software disc to within a pixel, the strict 2/255 comparison on the pixels away from the coverage edges,
// and the ink AREA (the summed blend weights) within a few percent of the software ink pixel count.

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::TextureOptions;
using Renderer::Software::SoftwareRenderer;
namespace Vk = Renderer::Gpu::ShaderInterface;

namespace
{
    /// Captures what is written to std::cerr (where the renderer logs) while it lives
    class CerrCapture
    {
    public:
        CerrCapture() : _old(std::cerr.rdbuf(_buffer.rdbuf())) {}
        ~CerrCapture() { std::cerr.rdbuf(_old); }
        CerrCapture(const CerrCapture&) = delete;
        CerrCapture& operator=(const CerrCapture&) = delete;

        [[nodiscard]] std::string Text() const { return _buffer.str(); }

    private:
        std::ostringstream _buffer;
        std::streambuf* _old;
    };

    int CountOccurrences(const std::string& text, const std::string& needle)
    {
        int count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        {
            ++count;
        }
        return count;
    }

    /// The UI pass's state: no depth test or write, no culling, blended
    RenderState Overlay()
    {
        RenderState state;
        state.depthTest = false;
        state.depthWrite = false;
        state.cull = CullMode::None;
        state.blend = true;
        return state;
    }

    /// The software renderer's frame of the disc scene: the green text quad drawn in the Transparent queue's state
    Frame SoftwareDiscFrame()
    {
        SoftwareRenderer software;
        EXPECT_TRUE(software.Initialize(nullptr, Size, Size));
        software.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        const std::vector<std::uint8_t> sdf = Ink::DiscSdf();
        Renderer::Common::ITexture* atlas =
            software.CreateTexture(sdf.data(), Ink::DiscSize, Ink::DiscSize, 1, TextureOptions::SdfAtlas());
        IMaterial* text = software.CreateMaterial(software.GetStandardTextShader(), atlas);
        IMesh* quad = software.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
        EXPECT_NE(atlas, nullptr);
        EXPECT_NE(text, nullptr);
        EXPECT_NE(quad, nullptr);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4), Size, Size};
        if (text != nullptr && quad != nullptr)
        {
            text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
            software.BeginFrame();
            software.SetViewProjection(Identity, Identity);
            software.DrawMesh(quad, Identity, text, RenderState::Transparent());
            software.EndFrame();
            software.Present();
            software.ReadFramebuffer(frame.rgba.data(), Size, Size);
        }
        software.Shutdown();
        return frame;
    }

    /// Pixels clearly inside the disc have `inside`, pixels clearly outside have `outside`; the ring within 1.5 pixels of the
    /// edge (the antialiasing ramp is under a pixel each side of it) may be anything
    void ExpectDisc(const Frame& frame, const Rgb inside, const Rgb outside)
    {
        int wrongInside = 0;
        int wrongOutside = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const float d = Ink::DistanceFromDiscCentre(x, y);
                if (d < Ink::DiscRadiusPx - 1.5f && !(frame.At(x, y) == inside))
                {
                    ++wrongInside;
                }
                if (d > Ink::DiscRadiusPx + 1.5f && !(frame.At(x, y) == outside))
                {
                    ++wrongOutside;
                }
            }
        }
        EXPECT_EQ(wrongInside, 0) << "pixels inside the disc without the inside colour";
        EXPECT_EQ(wrongOutside, 0) << "pixels outside the disc without the outside colour";
    }

    /// The disc's ink (>= 50% coverage): about pi r^2 pixels, in a box 2r pixels wide around the centre
    void ExpectDiscInk(const Frame& frame, const Rgb ink, const Rgb background)
    {
        constexpr float expectedCount = 3.14159265f * Ink::DiscRadiusPx * Ink::DiscRadiusPx; // ~314
        const Coverage c = Ink::InkCoverage(frame, ink, background);
        EXPECT_NEAR(static_cast<float>(c.count), expectedCount, expectedCount * 0.08f);
        EXPECT_NEAR(static_cast<float>(c.maxX - c.minX + 1), 2.0f * Ink::DiscRadiusPx, 2.0f);
        EXPECT_NEAR(static_cast<float>(c.maxY - c.minY + 1), 2.0f * Ink::DiscRadiusPx, 2.0f);
    }

    /// A text material fixture: an SDF disc atlas, a green text material, an opaque red unlit material and two quads
    class VulkanTextShaderTest : public VulkanDrawTest
    {
    protected:
        void SetUp() override
        {
            VulkanDrawTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            _textShader = R().GetStandardTextShader();
            ASSERT_NE(_textShader, nullptr);
            const std::vector<std::uint8_t> sdf = Ink::DiscSdf();
            _atlas = R().CreateTexture(sdf.data(), Ink::DiscSize, Ink::DiscSize, 1, TextureOptions::SdfAtlas());
            ASSERT_NE(_atlas, nullptr);
            _text = R().CreateMaterial(_textShader, _atlas);
            ASSERT_NE(_text, nullptr);
            _text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
            _opaqueRed = Colour(1.0f, 0.0f, 0.0f);
            _textQuad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
            _backQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f)); // full screen, behind the text
            ASSERT_NE(_opaqueRed, nullptr);
            ASSERT_NE(_textQuad, nullptr);
            ASSERT_NE(_backQuad, nullptr);
        }

        /// A text material over the fixture's atlas
        IMaterial* TextMaterial()
        {
            IMaterial* material = R().CreateMaterial(_textShader, _atlas);
            EXPECT_NE(material, nullptr);
            if (material != nullptr)
            {
                material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
            }
            return material;
        }

        Renderer::Common::IShader* _textShader = nullptr;
        ITexture* _atlas = nullptr;
        IMaterial* _text = nullptr;
        IMaterial* _opaqueRed = nullptr;
        IMesh* _textQuad = nullptr;
        IMesh* _backQuad = nullptr;
    };
}

// ============================================================================
// The seven cases of SoftwareTextShaderTest, with GL's expectations
// ============================================================================

TEST_F(VulkanTextShaderTest, AnSdfDiscIsAntialiasedAtTheEdge)
{
    const Frame frame = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });

    // The interior is exactly the text colour and the far background exactly untouched, as on the software renderer
    ExpectDisc(frame, kGreen, kBlack);
    ExpectDiscInk(frame, kGreen, kBlack);

    // The OPPOSITE of the software renderer's 'nothing in between' (SoftwareTextShaderTest.AnSdfDiscIsAlphaTestedAtTheEdge
    // asserts green + black == every pixel): the edge is antialiased, so a ring of in-between colours exists, and it is made
    // of the text colour blended with the background: green scaled, never another hue
    const int between = Ink::InBetween(frame, kGreen, kBlack);
    EXPECT_GE(between, 25) << "no antialiasing ring";
    EXPECT_LE(between, 200) << "a ring, not a smear";
    for (int y = 0; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            if (pixel == kGreen || pixel == kBlack)
            {
                continue;
            }
            const float d = Ink::DistanceFromDiscCentre(x, y);
            EXPECT_LE(std::abs(d - Ink::DiscRadiusPx), 1.5f) << "an in-between pixel away from the edge at (" << x << "," << y << ")";
            EXPECT_EQ(pixel.r, 0) << x << "," << y;
            EXPECT_EQ(pixel.b, 0) << x << "," << y;
        }
    }
}

TEST_F(VulkanTextShaderTest, TheDiscAtHalfCoverageIsTheSoftwareDiscWithinAPixelAndTheAreasAgree)
{
    const Frame frame = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const Frame software = SoftwareDiscFrame();
    ASSERT_EQ(software.width, Size);
    ExpectDiscInk(software, kGreen, kBlack);

    // The >= 50% coverage disc against the software (alpha-tested) disc: the two cut at the same 0.5 iso-line, so a pixel in
    // one and not the other lies within a pixel of the edge, and there are few of them
    const Frame thresholded = Ink::Thresholded(frame, kGreen, kBlack);
    int differing = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            if (!(thresholded.At(x, y) == software.At(x, y)))
            {
                ++differing;
                EXPECT_LE(std::abs(Ink::DistanceFromDiscCentre(x, y) - Ink::DiscRadiusPx), 1.0f)
                    << "the discs differ away from the edge at (" << x << "," << y << ")";
            }
        }
    }
    EXPECT_LE(differing, 12) << "pixels in one disc and not the other";

    // The strict comparison on the interior and the background, the coverage-edge pixels left out (edge border 1: the ramp is
    // under two pixels wide; the excluded share is the ring of about three pixels around a 63-pixel circle, a few percent of
    // the frame, capped at 15%), and the ink area against the software ink count (4%: the software count is a pixel-centre
    // count of a circle of radius 10, which itself differs from the area by about a percent)
    Ink::AntialiasedRule rule;
    rule.edgeBorder = 1;
    rule.maxExcludedFraction = 0.15;
    rule.maxAreaError = 0.04;
    EXPECT_TRUE(Ink::CompareAntialiased("text shader, an SDF disc", frame, software, kGreen, kBlack, rule));
}

TEST_F(VulkanTextShaderTest, TextWritesNoDepthSoAnOpaqueQuadDrawnBehindItLaterCoversTheWholeQuad)
{
    const Frame textOnly = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const Frame frame = RenderFrame(
        [&]
        {
            // In the Transparent queue's state (depth-tested, no depth write), then an opaque quad behind it
            R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent());
            R().DrawMesh(_backQuad, Identity, _opaqueRed, RenderState::Opaque());
        });

    EXPECT_GT(textOnly.Count(kGreen), 200) << "the text alone draws";
    // GL SEMANTICS. SoftwareTextShaderTest.TextWritesDepthSoAnOpaqueQuadDrawnBehindItLaterLeavesItAlone expects the glyph to
    // survive (the software text shader writes depth wherever the draw depth-tests, even with depthWrite off). OpenGL and
    // Vulkan write depth only as the state says, and the Transparent state writes none: the later opaque quad is not
    // rejected anywhere, and covers the whole frame
    EXPECT_EQ(frame.Count(kRed), Size * Size);
    EXPECT_EQ(frame.Count(kGreen), 0);
}

TEST_F(VulkanTextShaderTest, OtherTransparentDrawsWriteNoDepth)
{
    // The contrast, as on the software renderer: an unlit draw in the same state is covered by the quad behind it
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    ASSERT_NE(green, nullptr);
    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(_textQuad, Identity, green, RenderState::Transparent());
            R().DrawMesh(_backQuad, Identity, _opaqueRed, RenderState::Opaque());
        });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
}

TEST_F(VulkanTextShaderTest, WithoutADepthTestTextWritesNoDepth)
{
    // As in OpenGL, nothing writes depth while the depth test is off (the UI pass's state)
    const RenderState overlay = Overlay();
    const Frame overlayOnly = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, overlay); });
    ExpectDiscInk(overlayOnly, kGreen, kBlack);
    ExpectDisc(overlayOnly, kGreen, kBlack);

    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(_textQuad, Identity, _text, overlay);
            R().DrawMesh(_backQuad, Identity, _opaqueRed, RenderState::Opaque());
        });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
}

TEST_F(VulkanTextShaderTest, ColourIsAlbedoTimesVertexColour)
{
    // Yellow albedo times a cyan vertex colour is green
    _text->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f);
    IMesh* cyanQuad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    ASSERT_NE(cyanQuad, nullptr);

    const Frame frame = RenderFrame([&] { R().DrawMesh(cyanQuad, Identity, _text, RenderState::Transparent()); });
    ExpectDiscInk(frame, kGreen, kBlack);
    ExpectDisc(frame, kGreen, kBlack);
}

TEST_F(VulkanTextShaderTest, FullyTransparentTextDrawsNothing)
{
    // OpenGL discards below alpha 0.01, and so does this shader: no colour AND no depth. The depth half is only visible
    // with a draw that writes depth, so the text is drawn in the Opaque state (depth written, not blended) and an opaque
    // quad behind it is drawn after
    _text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 0.0f);

    const Frame textOnly = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    EXPECT_EQ(textOnly.Count(kBlack), Size * Size) << "no colour written";

    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(_textQuad, Identity, _text, RenderState::Opaque());
            R().DrawMesh(_backQuad, Identity, _opaqueRed, RenderState::Opaque());
        });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "no colour and no depth written";

    // Just above the threshold something is drawn: alpha 0.02 gives the interior a faint (0.02) green
    _text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 0.02f);
    const Frame faint = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const Rgb centre = faint.At(Size / 2, Size / 2);
    EXPECT_GT(centre.g, 0) << "alpha 0.02 is above the discard threshold";
    EXPECT_LE(centre.g, 8);
}

TEST_F(VulkanTextShaderTest, TextWithoutAnAtlasDrawsNothingAndSoDoesTextWhoseAtlasWasDestroyed)
{
    // GL binds the base texture only when it is valid, so a text material without one draws nothing. Here the base sampler's
    // default for a text material is a renderer-owned 1x1 (0,0,0,0) texture (sdf 0: coverage 0, discarded), not the white
    // default of the unlit and lit shaders, which would read sdf 1 and fill the whole quad. Nothing is written: not colour
    // and, drawn in a depth-writing state, not depth either
    const auto drawsNothing = [&](IMaterial* material, const std::string& what)
    {
        const Frame colour = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, material, RenderState::Transparent()); });
        EXPECT_EQ(colour.Count(kBlack), Size * Size) << what << ": colour written";
        const Frame depth = RenderFrame(
            [&]
            {
                R().DrawMesh(_textQuad, Identity, material, RenderState::Opaque());
                R().DrawMesh(_backQuad, Identity, _opaqueRed, RenderState::Opaque());
            });
        EXPECT_EQ(depth.Count(kRed), Size * Size) << what << ": depth written";
    };

    // 1. No atlas at creation
    IMaterial* noAtlas = R().CreateMaterial(_textShader);
    ASSERT_NE(noAtlas, nullptr);
    noAtlas->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    EXPECT_EQ(noAtlas->GetTexture(), nullptr);
    drawsNothing(noAtlas, "no atlas");

    // 2. The atlas given later draws, and taking it away again draws nothing
    noAtlas->SetTexture(_atlas);
    EXPECT_GT(RenderFrame([&] { R().DrawMesh(_textQuad, Identity, noAtlas, RenderState::Transparent()); }).Count(kGreen), 200);
    noAtlas->SetTexture(nullptr);
    drawsNothing(noAtlas, "atlas set back to null");

    // 3. The atlas destroyed after SetTexture: the material still points at it, the renderer no longer has it
    const std::vector<std::uint8_t> sdf = Ink::DiscSdf();
    ITexture* doomed = R().CreateTexture(sdf.data(), Ink::DiscSize, Ink::DiscSize, 1, TextureOptions::SdfAtlas());
    ASSERT_NE(doomed, nullptr);
    IMaterial* material = R().CreateMaterial(_textShader, doomed);
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    EXPECT_GT(RenderFrame([&] { R().DrawMesh(_textQuad, Identity, material, RenderState::Transparent()); }).Count(kGreen), 200)
        << "the atlas draws while it lives";
    R().DestroyTexture(doomed);
    drawsNothing(material, "atlas destroyed after SetTexture");

    // A texture made at the destroyed one's address is not mistaken for it either: the material draws nothing again
    ITexture* later = R().CreateTexture(sdf.data(), Ink::DiscSize, Ink::DiscSize, 1, TextureOptions::SdfAtlas());
    ASSERT_NE(later, nullptr);
    drawsNothing(material, "a new texture where the destroyed atlas was");
}

// ============================================================================
// Vulkan-only (what antialiasing and blending add)
// ============================================================================

TEST_F(VulkanTextShaderTest, TranslucentTextBlendsWithWhatIsBehindItByGLsEquation)
{
    // The clear colour is blue, the text half-transparent green: every interior pixel is half green over blue, and every
    // edge pixel a mix of the three along the way (green + blue always about 255: the weights sum to one)
    ClearTo(R(), {0, 0, 255, 255});
    _text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 0.5f);
    const Frame frame = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const int expectedG = Blend::BlendChannel(255, 0, 0.5, false);
    const int expectedB = Blend::BlendChannel(0, 255, 0.5, false);
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            const float d = Ink::DistanceFromDiscCentre(x, y);
            if (d < Ink::DiscRadiusPx - 1.5f)
            {
                EXPECT_NEAR(pixel.g, expectedG, 1) << x << "," << y;
                EXPECT_NEAR(pixel.b, expectedB, 1) << x << "," << y;
            }
            else if (d > Ink::DiscRadiusPx + 1.5f)
            {
                EXPECT_EQ(pixel, (Rgb{0, 0, 255})) << x << "," << y;
            }
            EXPECT_EQ(pixel.r, 0);
            EXPECT_NEAR(static_cast<int>(pixel.g) + pixel.b, 255, 2) << "the weights sum to one at " << x << "," << y;
        }
    }
}

TEST_F(VulkanTextShaderTest, SoftnessWidensTheRampAroundTheSameIsoLine)
{
    IMaterial* soft = TextMaterial();
    ASSERT_NE(soft, nullptr);
    // 0.1 in atlas value units is a ramp half-width of 1.6 pixels here (the disc's distance changes by 16/255 per pixel)
    soft->SetFloat("uSoftness", 0.1f);

    const Frame sharp = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const Frame wide = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, soft, RenderState::Transparent()); });

    const int sharpRing = Ink::InBetween(sharp, kGreen, kBlack);
    const int wideRing = Ink::InBetween(wide, kGreen, kBlack);
    EXPECT_GT(wideRing, sharpRing * 3 / 2) << "softness widens the ramp: more in-between pixels";
    EXPECT_GT(wideRing, sharpRing + 40);
    // The same 50% iso-line: the ink (>= half coverage) is the same disc, to a few pixels
    EXPECT_NEAR(Ink::InkPixels(wide, kGreen, kBlack), Ink::InkPixels(sharp, kGreen, kBlack), 12);
    // and the same area: the ramp is symmetric around the iso-line
    EXPECT_NEAR(Ink::InkArea(wide, kGreen, kBlack), Ink::InkArea(sharp, kGreen, kBlack), 0.04 * Ink::InkArea(sharp, kGreen, kBlack));
    // beyond the widened ramp it is solid and untouched as before
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const float d = Ink::DistanceFromDiscCentre(x, y);
            if (d < Ink::DiscRadiusPx - 2.5f)
            {
                EXPECT_EQ(wide.At(x, y), kGreen) << x << "," << y;
            }
            else if (d > Ink::DiscRadiusPx + 2.5f)
            {
                EXPECT_EQ(wide.At(x, y), kBlack) << x << "," << y;
            }
        }
    }
}

TEST_F(VulkanTextShaderTest, AnOutlineIsARingOutsideTheFaceAndLeavesTheFaceAlone)
{
    IMaterial* outlined = TextMaterial();
    ASSERT_NE(outlined, nullptr);
    // The outline's outer edge is at 0.5 - uOutline: 0.1 is 1.6 pixels outside the face's edge here
    outlined->SetFloat("uOutline", 0.1f);
    outlined->SetColor("uOutlineColor", 0.0f, 0.0f, 1.0f, 1.0f);

    const Frame plain = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, RenderState::Transparent()); });
    const Frame frame = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, outlined, RenderState::Transparent()); });

    int blueRing = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            const float d = Ink::DistanceFromDiscCentre(x, y);
            if (d < Ink::DiscRadiusPx - 1.5f)
            {
                EXPECT_EQ(pixel, kGreen) << "the face is untouched at " << x << "," << y;
            }
            if (d > Ink::DiscRadiusPx + 2.6f)
            {
                EXPECT_EQ(pixel, kBlack) << "nothing beyond the outline at " << x << "," << y;
            }
            if (pixel.b >= 200 && pixel.r <= 40 && pixel.g <= 40)
            {
                ++blueRing;
                EXPECT_GE(d, Ink::DiscRadiusPx + 0.2f) << "outline colour inside the face at " << x << "," << y;
                EXPECT_LE(d, Ink::DiscRadiusPx + 2.6f) << "outline colour too far out at " << x << "," << y;
            }
        }
    }
    EXPECT_GE(blueRing, 15) << "no outline ring";
    // The face (>= half coverage of green) is where it was, give or take a few edge pixels
    const auto faceOf = [](const Frame& f)
    {
        int count = 0;
        for (int y = 0; y < f.height; ++y)
        {
            for (int x = 0; x < f.width; ++x)
            {
                count += f.At(x, y).g >= 128 ? 1 : 0;
            }
        }
        return count;
    };
    EXPECT_NEAR(faceOf(frame), faceOf(plain), 12);
    EXPECT_GT(Ink::InBetween(frame, kGreen, kBlack), 0);
}

TEST_F(VulkanTextShaderTest, AShadowIsOffsetUnderTheFace)
{
    IMaterial* shadowed = TextMaterial();
    ASSERT_NE(shadowed, nullptr);
    // The shadow at uv is the shape at uv - offset: the shape moved by the offset. 5/32 of the quad right, 2/32 down (v is up
    // over this quad), which is 5 pixels right and 2 down
    shadowed->SetColor("uShadowColor", 1.0f, 0.0f, 0.0f, 1.0f);
    shadowed->SetVec2("uShadowOffset", 5.0f / 32.0f, -2.0f / 32.0f);

    const Frame frame = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, shadowed, RenderState::Transparent()); });
    constexpr float shadowX = Ink::DiscCentrePx + 5.0f;
    constexpr float shadowY = Ink::DiscCentrePx - 2.0f;
    int redRing = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            const float face = Ink::DistanceFromDiscCentre(x, y);
            const float shadow = std::hypot(static_cast<float>(x) + 0.5f - shadowX, static_cast<float>(y) + 0.5f - shadowY);
            if (face < Ink::DiscRadiusPx - 1.5f)
            {
                EXPECT_EQ(pixel, kGreen) << "the face is on top of the shadow at " << x << "," << y;
            }
            else if (shadow < Ink::DiscRadiusPx - 1.5f && face > Ink::DiscRadiusPx + 1.5f)
            {
                EXPECT_EQ(pixel, kRed) << "the shadow's interior, outside the face, at " << x << "," << y;
            }
            else if (shadow > Ink::DiscRadiusPx + 1.5f && face > Ink::DiscRadiusPx + 1.5f)
            {
                EXPECT_EQ(pixel, kBlack) << "outside both at " << x << "," << y;
            }
            if (pixel.r >= 200 && pixel.g <= 40 && pixel.b <= 40)
            {
                ++redRing;
                EXPECT_GE(face, Ink::DiscRadiusPx) << "shadow colour inside the face at " << x << "," << y;
            }
        }
    }
    EXPECT_GE(redRing, 25) << "no shadow";
}

TEST_F(VulkanTextShaderTest, OneMaterialDrawnTwiceInOneFrameWithDifferentValuesDrawsEachWithItsOwn)
{
    // Each DrawMesh takes a snapshot of the material's block and textures into the frame's ring, so a value changed between
    // two draws of one frame affects only the draws after it: the path the engine's multi-pass text effects take (one
    // material and mesh, the effect uniforms changed between DrawMesh calls)
    IMesh* left = MakeMesh(Quad(-1.0f, -0.5f, 0.0f, 0.5f, 0.0f));
    IMesh* right = MakeMesh(Quad(0.0f, -0.5f, 1.0f, 0.5f, 0.0f));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    const Frame frame = RenderFrame(
        [&]
        {
            _text->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
            R().DrawMesh(left, Identity, _text, RenderState::Transparent());
            _text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
            R().DrawMesh(right, Identity, _text, RenderState::Transparent());
            _text->SetColor("uAlbedo", 0.0f, 0.0f, 1.0f, 1.0f); // after the last draw: affects none
        });
    EXPECT_EQ(frame.At(16, 32), kRed) << "the first draw has the first value";
    EXPECT_EQ(frame.At(48, 32), kGreen) << "the second draw has the second";
    EXPECT_EQ(frame.Count(kBlue), 0) << "a value set after the last draw did not reach it";
    EXPECT_GT(frame.Count(kRed), 200);
    EXPECT_GT(frame.Count(kGreen), 200);

    // A text material is not shared with draws of the next frame either: the snapshot is the value at the draw
    const Frame next = RenderFrame([&] { R().DrawMesh(left, Identity, _text, RenderState::Transparent()); });
    EXPECT_EQ(next.At(16, 32), kBlue);
}

TEST_F(VulkanTextShaderTest, EffectPassesAreDrawnWithTheirOwnUniformsAndTheFaceLast)
{
    // The sequence TextDrawing::DrawPasses draws: a pass is the shadow of a face that is not drawn (albedo alpha 0, only
    // uShadowColor / uShadowOffset / uShadowSoftness / the width in uOutline set), then the face with every effect off
    IMaterial* material = TextMaterial();
    ASSERT_NE(material, nullptr);
    const auto drawPass = [&]
    {
        material->SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 0.0f);
        material->SetFloat("uOutline", 0.0f);
        material->SetColor("uOutlineColor", 0.0f, 0.0f, 0.0f, 0.0f);
        material->SetFloat("uSoftness", 0.0f);
        material->SetColor("uShadowColor", 1.0f, 0.0f, 0.0f, 1.0f);
        material->SetVec2("uShadowOffset", 5.0f / 32.0f, 0.0f);
        material->SetFloat("uShadowSoftness", 0.0f);
        R().DrawMesh(_textQuad, Identity, material, RenderState::Transparent());
    };
    const auto drawFace = [&]
    {
        material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
        material->SetFloat("uOutline", 0.0f);
        material->SetColor("uOutlineColor", 0.0f, 0.0f, 0.0f, 0.0f);
        material->SetFloat("uSoftness", 0.0f);
        material->SetColor("uShadowColor", 0.0f, 0.0f, 0.0f, 0.0f);
        material->SetVec2("uShadowOffset", 0.0f, 0.0f);
        material->SetFloat("uShadowSoftness", 0.0f);
        R().DrawMesh(_textQuad, Identity, material, RenderState::Transparent());
    };
    const Frame passes = RenderFrame(
        [&]
        {
            drawPass();
            drawFace();
        });
    const Frame faceOnly = RenderFrame([&] { drawFace(); });
    const Frame passOnly = RenderFrame([&] { drawPass(); });

    // The face pass leaves the pass's red outside the face: pure green inside, red where only the shadow reaches
    int redOutsideFace = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const float face = Ink::DistanceFromDiscCentre(x, y);
            const float shadow = std::hypot(static_cast<float>(x) + 0.5f - (Ink::DiscCentrePx + 5.0f),
                                            static_cast<float>(y) + 0.5f - Ink::DiscCentrePx);
            if (face < Ink::DiscRadiusPx - 1.5f)
            {
                EXPECT_EQ(passes.At(x, y), kGreen) << "the face is last, over the pass, at " << x << "," << y;
            }
            else if (shadow < Ink::DiscRadiusPx - 1.5f && face > Ink::DiscRadiusPx + 1.5f)
            {
                EXPECT_EQ(passes.At(x, y), kRed) << "the pass, outside the face, at " << x << "," << y;
                ++redOutsideFace;
            }
        }
    }
    EXPECT_GT(redOutsideFace, 10) << "the pass shows beside the face (about 22 pixels where it is solid red)";
    // The pass alone has no green at all (its face alpha is 0), and the face alone no red: neither pass's values reached the
    // other's draw
    EXPECT_EQ(passOnly.Count(kGreen), 0);
    EXPECT_GT(passOnly.Count(kRed), 200) << "the whole shadow disc, face included, is the pass's";
    EXPECT_EQ(faceOnly.Count(kRed), 0);

    // One draw of the same face and shadow (the engine's single-draw settings path) composites them in the shader; the two
    // passes blend them one after the other: the same image, to a rounding step
    IMaterial* single = TextMaterial();
    ASSERT_NE(single, nullptr);
    single->SetColor("uShadowColor", 1.0f, 0.0f, 0.0f, 1.0f);
    single->SetVec2("uShadowOffset", 5.0f / 32.0f, 0.0f);
    const Frame oneDraw = RenderFrame([&] { R().DrawMesh(_textQuad, Identity, single, RenderState::Transparent()); });
    const Ink::FrameDifference difference = Ink::Difference(passes, oneDraw, 3);
    EXPECT_FALSE(difference.sizesDiffer);
    EXPECT_EQ(difference.overTolerance, 0) << "the passes and the single draw differ by up to " << difference.maxChannel << "/255";
}

TEST_F(VulkanTextShaderTest, ATextMeshUpdatedEveryFrameOverMoreFramesThanAreInFlightIsValidationCleanAndFollowsTheUpdates)
{
    // TextDrawing UpdateMesh's the text mesh in place whenever the layout changes. A new buffer is made per update and the
    // old one retired to the frame's deletion queue; the fixture fails the test on any validation message or a leak
    IMesh* mesh = MakeMesh(Quad(-1.0f, -0.5f, 0.0f, 0.5f, 0.0f));
    ASSERT_NE(mesh, nullptr);
    for (int frameIndex = 0; frameIndex < 11; ++frameIndex)
    {
        const float x0 = -1.0f + 0.1f * static_cast<float>(frameIndex);
        ASSERT_TRUE(R().UpdateMesh(mesh, Quad(x0, -0.5f, x0 + 1.0f, 0.5f, 0.0f)));
        const Frame frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, _text, RenderState::Transparent()); });
        const Coverage c = Ink::InkCoverage(frame, kGreen, kBlack);
        ASSERT_GT(c.count, 250) << "frame " << frameIndex;
        // The quad is 32 px wide and its disc is 20 px across in the middle: the disc's centre follows the quad's
        const float expectedCentre = (x0 + 1.5f) * 32.0f;
        EXPECT_NEAR(static_cast<float>(c.minX + c.maxX + 1) * 0.5f, expectedCentre, 1.0f) << "frame " << frameIndex;
    }
    // The retired buffers are freed once the frame slots are flushed
    for (int i = 0; i < 3; ++i)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().EndFrame();
    }
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
}

TEST_F(VulkanTextShaderTest, TheTextPipelineInTransparentAndOverlayStatesIsCached)
{
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    const auto drawIn = [&](const RenderState& state)
    {
        RenderFrame([&] { R().DrawMesh(_textQuad, Identity, _text, state); });
    };
    drawIn(RenderState::Transparent());
    EXPECT_EQ(R().GetPipelineCountForTest(), 1u) << "the text pipeline in the Transparent queue's state";
    drawIn(RenderState::Transparent());
    EXPECT_EQ(R().GetPipelineCountForTest(), 1u) << "made once, found again";
    drawIn(Overlay());
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u) << "the UI pass's state is another pipeline";
    drawIn(Overlay());
    drawIn(RenderState::Transparent());
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u);
    // Two materials of the text shader share the pipeline
    IMaterial* other = TextMaterial();
    RenderFrame([&] { R().DrawMesh(_textQuad, Identity, other, RenderState::Transparent()); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u);
}

// ============================================================================
// Text materials
// ============================================================================

TEST_F(VulkanTextShaderTest, ATextMaterialCarriesExactlyTheGlDefaultsThroughTheNameTable)
{
    IMaterial* material = R().CreateMaterial(_textShader, _atlas);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->GetTexture(), _atlas);
    std::vector<std::uint8_t> block;
    ASSERT_TRUE(R().GetMaterialBlockForTest(material, block));

    const auto floatsOf = [](const Vk::UniformType type)
    {
        switch (type)
        {
        case Vk::UniformType::Vec2: return 2;
        case Vk::UniformType::Vec3: return 3;
        case Vk::UniformType::Vec4: return 4;
        default: return 1;
        }
    };
    // Every default of the table (uAlbedo white, the six effect uniforms 0), read back at the name table's offset
    for (const Vk::MaterialDefault& entry : Vk::kTextMaterialDefaults)
    {
        const Vk::Uniform* uniform = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, entry.name);
        ASSERT_NE(uniform, nullptr) << entry.name;
        const int count = floatsOf(uniform->type);
        ASSERT_LE(uniform->offset + count * sizeof(float), block.size()) << entry.name;
        for (int i = 0; i < count; ++i)
        {
            float stored = -1.0f;
            std::memcpy(&stored, block.data() + uniform->offset + static_cast<std::size_t>(i) * sizeof(float), sizeof(float));
            EXPECT_EQ(stored, entry.values[static_cast<std::size_t>(i)]) << entry.name << "[" << i << "]";
        }
    }

    // A value set by name lands at the table's offset, and only there
    const Vk::Uniform* outline = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, "uOutline");
    const Vk::Uniform* shadowOffset = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, "uShadowOffset");
    ASSERT_NE(outline, nullptr);
    ASSERT_NE(shadowOffset, nullptr);
    material->SetFloat("uOutline", 0.25f);
    material->SetVec2("uShadowOffset", 0.5f, -0.75f);
    material->SetColor("uShadowColor", 0.1f, 0.2f, 0.3f, 0.4f);
    std::vector<std::uint8_t> changed;
    ASSERT_TRUE(R().GetMaterialBlockForTest(material, changed));
    float value = 0.0f;
    std::memcpy(&value, changed.data() + outline->offset, sizeof(float));
    EXPECT_EQ(value, 0.25f);
    std::memcpy(&value, changed.data() + shadowOffset->offset, sizeof(float));
    EXPECT_EQ(value, 0.5f);
    std::memcpy(&value, changed.data() + shadowOffset->offset + sizeof(float), sizeof(float));
    EXPECT_EQ(value, -0.75f);
    const Vk::Uniform* shadowColor = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, "uShadowColor");
    ASSERT_NE(shadowColor, nullptr);
    std::memcpy(&value, changed.data() + shadowColor->offset + 3 * sizeof(float), sizeof(float));
    EXPECT_EQ(value, 0.4f);
    // Nothing else changed
    std::size_t differing = 0;
    for (std::size_t i = 0; i < block.size(); ++i)
    {
        differing += block[i] != changed[i] ? 1 : 0;
    }
    EXPECT_GT(differing, 0u);
    EXPECT_LE(differing, 4u * (1 + 2 + 4));

    // The materials of foreign or null shaders are still refused
    EXPECT_EQ(R().CreateMaterial(nullptr), nullptr);
    EXPECT_EQ(R().CreateMaterial(nullptr, _atlas), nullptr);
}

TEST_F(VulkanTextShaderTest, ANameThatIsNotInTheTextBlockIsLoggedOnceAndIgnored)
{
    IMaterial* material = TextMaterial();
    ASSERT_NE(material, nullptr);
    std::vector<std::uint8_t> before;
    ASSERT_TRUE(R().GetMaterialBlockForTest(material, before));

    const CerrCapture capture;
    material->SetFloat("uMetallic", 1.0f); // a lit-only name
    material->SetFloat("uMetallic", 0.5f);
    material->SetInt("uHasTexture", 0);    // an unlit/lit name
    material->SetFloat("uShadowOffset", 1.0f); // the right name, the wrong type
    material->SetFloat("uShadowOffset", 2.0f);
    EXPECT_EQ(CountOccurrences(capture.Text(), "'uMetallic' is not in the text shader's material block"), 1) << capture.Text();
    EXPECT_EQ(CountOccurrences(capture.Text(), "'uHasTexture' is not in the text shader's material block"), 1) << capture.Text();
    EXPECT_EQ(CountOccurrences(capture.Text(), "'uShadowOffset' is a vec2, so SetFloat does not apply to it"), 1)
        << capture.Text();

    std::vector<std::uint8_t> after;
    ASSERT_TRUE(R().GetMaterialBlockForTest(material, after));
    EXPECT_EQ(before, after) << "an ignored name changes nothing";
}

TEST_F(VulkanTextShaderTest, CustomGlslIsStillRefusedWithOneWarning)
{
    const CerrCapture capture;
    EXPECT_EQ(R().CreateShaderProgram("void main(){}", "void main(){}"), nullptr);
    EXPECT_EQ(R().CreateShaderProgram("void main(){}", "void main(){}"), nullptr);
    EXPECT_EQ(CountOccurrences(capture.Text(), "custom GLSL shaders are not supported by the Vulkan backend"), 1)
        << capture.Text();
    EXPECT_FALSE(R().DestroyShaderProgram(_textShader)) << "the text built-in belongs to the renderer";
    EXPECT_TRUE(R().IsValidShader(_textShader));
}
