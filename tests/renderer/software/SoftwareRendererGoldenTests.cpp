#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// Golden-image tests of the software renderer in headless mode (Initialize(nullptr, ...)): real frames
// rasterized on the render thread and read back with ReadFramebuffer, without a window or GPU. They check
// structure (pixel counts, bounding boxes, which pixels are untouched), with tolerances, never exact
// images or hashes: Debug and Release may round floats differently.

using Renderer::Common::CullMode;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::TextureOptions;
using Renderer::Common::Vertex;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Width = 64;
    constexpr int Height = 64;

    constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    constexpr Rgb Black{0, 0, 0};
    constexpr Rgb Green{0, 255, 0};
    constexpr Rgb Red{255, 0, 0};

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row, like NDC y
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] std::uint8_t AlphaAt(const int x, const int y) const
        {
            return rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x)) * 4 + 3];
        }
    };

    struct Coverage
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    /// The pixels of one colour: how many, and their bounding box
    Coverage CoverageOf(const Frame &frame, const Rgb colour)
    {
        Coverage c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (frame.At(x, y) == colour)
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

    /// An axis-aligned quad in NDC (the tests draw with identity matrices) at depth z, wound
    /// counter-clockwise, with uvs 0..1 over it and one vertex colour
    MeshData Quad(const float x0, const float y0, const float x1, const float y1, const float z,
                  const float r = 1.0f, const float g = 1.0f, const float b = 1.0f, const float a = 1.0f)
    {
        MeshData data;
        data.vertices = {
            Vertex{{x0, y0, z}, {0, 0, 1}, {0, 0}, {r, g, b, a}},
            Vertex{{x1, y0, z}, {0, 0, 1}, {1, 0}, {r, g, b, a}},
            Vertex{{x1, y1, z}, {0, 0, 1}, {1, 1}, {r, g, b, a}},
            Vertex{{x0, y1, z}, {0, 0, 1}, {0, 1}, {r, g, b, a}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    // The synthetic SDF: a disc of radius DiscRadius texels in the middle of a DiscSize-texel square atlas,
    // edge at 128 (0.5), 16 levels per texel. Drawn over the quad below, one texel covers one pixel.
    constexpr int DiscSize = 32;
    constexpr float DiscRadius = 10.0f;

    std::vector<std::uint8_t> DiscSdf()
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(DiscSize) * DiscSize);
        constexpr float centre = DiscSize * 0.5f;
        for (int y = 0; y < DiscSize; ++y)
        {
            for (int x = 0; x < DiscSize; ++x)
            {
                const float dist = std::hypot(static_cast<float>(x) + 0.5f - centre,
                                              static_cast<float>(y) + 0.5f - centre);
                const float value = std::clamp(128.0f + (DiscRadius - dist) * 16.0f, 0.0f, 255.0f);
                pixels[static_cast<std::size_t>(y) * DiscSize + static_cast<std::size_t>(x)] =
                    static_cast<std::uint8_t>(value);
            }
        }
        return pixels;
    }

    // The text quad spans NDC -0.5..0.5, pixels 16..48 of 64: the disc's centre is pixel (32, 32), and its
    // radius, 10 texels, is 10 pixels
    constexpr float DiscCentrePx = 32.0f;
    constexpr float DiscRadiusPx = DiscRadius * (Width * 0.5f) / DiscSize;

    /// A headless renderer with an SDF disc atlas, a text material and an opaque unlit material
    struct Fixture
    {
        SoftwareRenderer renderer;
        ITexture *atlas = nullptr;
        IMaterial *text = nullptr;
        IMaterial *opaqueRed = nullptr;
        IMesh *textQuad = nullptr;
        IMesh *backQuad = nullptr; // full screen, behind the text

        bool Initialize()
        {
            if (!renderer.Initialize(nullptr, Width, Height))
            {
                return false;
            }
            renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

            const std::vector<std::uint8_t> sdf = DiscSdf();
            atlas = renderer.CreateTexture(sdf.data(), DiscSize, DiscSize, 1, TextureOptions::SdfAtlas());
            text = renderer.CreateMaterial(renderer.GetStandardTextShader(), atlas);
            opaqueRed = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            textQuad = renderer.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
            backQuad = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
            if (!atlas || !text || !opaqueRed || !textQuad || !backQuad)
            {
                return false;
            }
            text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
            opaqueRed->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
            return true;
        }

        /// Runs one frame of draws (identity view and projection) and reads it back
        template <typename Draws>
        Frame Render(Draws &&draws)
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draws();
            renderer.EndFrame();
            renderer.Present(); // headless: only waits for the frame

            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
            renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);
            return frame;
        }

        ~Fixture() { renderer.Shutdown(); }
    };

    float DistanceFromDiscCentre(const int x, const int y)
    {
        return std::hypot(static_cast<float>(x) + 0.5f - DiscCentrePx, static_cast<float>(y) + 0.5f - DiscCentrePx);
    }

    /// Pixels clearly inside the disc have `inside`, pixels clearly outside have `outside`; the two pixels
    /// either side of the edge may be either
    void ExpectDisc(const Frame &frame, const Rgb inside, const Rgb outside)
    {
        int wrongInside = 0;
        int wrongOutside = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const float d = DistanceFromDiscCentre(x, y);
                if (d < DiscRadiusPx - 1.5f && frame.At(x, y) != inside)
                {
                    ++wrongInside;
                }
                if (d > DiscRadiusPx + 1.5f && frame.At(x, y) != outside)
                {
                    ++wrongOutside;
                }
            }
        }
        EXPECT_EQ(wrongInside, 0) << "pixels inside the disc without the inside colour";
        EXPECT_EQ(wrongOutside, 0) << "pixels outside the disc without the outside colour";
    }

    /// The disc's coverage: about pi r^2 pixels, in a box 2r pixels wide around the centre
    void ExpectDiscCoverage(const Coverage &c)
    {
        constexpr float expectedCount = 3.14159265f * DiscRadiusPx * DiscRadiusPx; // ~314
        EXPECT_NEAR(static_cast<float>(c.count), expectedCount, expectedCount * 0.08f);
        // Pixel centres within the radius: 22..41 on each axis
        EXPECT_NEAR(c.minX, 22, 1);
        EXPECT_NEAR(c.maxX, 41, 1);
        EXPECT_NEAR(c.minY, 22, 1);
        EXPECT_NEAR(c.maxY, 41, 1);
    }
}

// ============================================================================
// Headless mode
// ============================================================================

TEST(SoftwareHeadlessTest, InitializesWithoutAWindowAndReadsBackTheClearColour)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 8, 4));
    renderer.Clear(0.0f, 0.0f, 1.0f, 1.0f);

    renderer.BeginFrame();
    renderer.EndFrame();
    renderer.Present();

    std::vector<std::uint8_t> pixels(8 * 4 * 4, 7);
    renderer.ReadFramebuffer(pixels.data(), 8, 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        EXPECT_EQ(pixels[i + 0], 0) << i;
        EXPECT_EQ(pixels[i + 1], 0) << i;
        EXPECT_EQ(pixels[i + 2], 255) << i;
        EXPECT_EQ(pixels[i + 3], 255) << i;
    }

    renderer.Shutdown();
    renderer.Shutdown(); // twice is safe
}

TEST(SoftwareHeadlessTest, ShutdownWithoutInitializeAndDestructionWithoutShutdownAreSafe)
{
    {
        SoftwareRenderer renderer;
        renderer.Shutdown();
    }
    {
        // A frame still in flight when the renderer is destroyed
        SoftwareRenderer renderer;
        ASSERT_TRUE(renderer.Initialize(nullptr, 16, 16));
        renderer.BeginFrame();
        renderer.EndFrame();
    }
    SUCCEED();
}

TEST(SoftwareHeadlessTest, ReadFramebufferIsRgbaWithTheBottomRowFirst)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    IMaterial *red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(red, nullptr);
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    IMesh *bottomHalf = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 0.0f, 0.0f));
    ASSERT_NE(bottomHalf, nullptr);

    renderer.BeginFrame();
    renderer.SetViewProjection(Identity, Identity);
    renderer.DrawMesh(bottomHalf, Identity, red);
    renderer.EndFrame();
    // No Present: ReadFramebuffer waits for the frame itself
    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
    renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);

    // NDC y -1..0 is the bottom half of the image, which is the first half of the buffer
    const Coverage c = CoverageOf(frame, Red);
    EXPECT_EQ(c.count, Width * Height / 2);
    EXPECT_EQ(c.minY, 0);
    EXPECT_EQ(c.maxY, Height / 2 - 1);
    EXPECT_EQ(frame.rgba[0], 255) << "R first";
    EXPECT_EQ(frame.rgba[2], 0) << "then G and B";
    EXPECT_EQ(frame.AlphaAt(0, 0), 255);

    renderer.Shutdown();
}

// ============================================================================
// The text shader
// ============================================================================

TEST(SoftwareTextShaderTest, AnSdfDiscIsAlphaTestedAtTheEdge)
{
    Fixture f;
    ASSERT_TRUE(f.Initialize());

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, RenderState::Transparent());
    });

    const Coverage green = CoverageOf(frame, Green);
    ExpectDiscCoverage(green);
    // Alpha-tested, not blended: every pixel is the text colour or untouched, nothing in between
    EXPECT_EQ(green.count + CoverageOf(frame, Black).count, Width * Height);
    ExpectDisc(frame, Green, Black);
}

TEST(SoftwareTextShaderTest, TextWritesDepthSoAnOpaqueQuadDrawnBehindItLaterLeavesItAlone)
{
    Fixture f;
    ASSERT_TRUE(f.Initialize());

    const Frame textOnly = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, RenderState::Transparent());
    });
    const Frame frame = f.Render([&] {
        // In the Transparent queue's state (no depth write), then an opaque quad behind it. The opaque
        // draw comes later and isn't moved before the text (OrderDraws keeps transparent draws in place).
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, RenderState::Transparent());
        f.renderer.DrawMesh(f.backQuad, Identity, f.opaqueRed, RenderState::Opaque());
    });

    // The glyph survives exactly; only its covered pixels wrote depth, so the rest of its quad is red
    EXPECT_EQ(CoverageOf(frame, Green).count, CoverageOf(textOnly, Green).count);
    EXPECT_EQ(CoverageOf(frame, Green).count + CoverageOf(frame, Red).count, Width * Height);
    ExpectDisc(frame, Green, Red);
}

TEST(SoftwareTextShaderTest, OtherTransparentDrawsWriteNoDepth)
{
    // The contrast: an unlit draw in the same state is covered by the quad behind it
    Fixture f;
    ASSERT_TRUE(f.Initialize());
    IMaterial *green = f.renderer.CreateMaterial(f.renderer.GetStandardUnlitShader());
    ASSERT_NE(green, nullptr);
    green->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, green, RenderState::Transparent());
        f.renderer.DrawMesh(f.backQuad, Identity, f.opaqueRed, RenderState::Opaque());
    });
    EXPECT_EQ(CoverageOf(frame, Red).count, Width * Height);
}

TEST(SoftwareTextShaderTest, WithoutADepthTestTextWritesNoDepth)
{
    // As in OpenGL, nothing writes depth while the depth test is off (the UI pass's state)
    Fixture f;
    ASSERT_TRUE(f.Initialize());
    RenderState overlay;
    overlay.depthTest = false;
    overlay.depthWrite = false;
    overlay.cull = CullMode::None;

    const Frame overlayOnly = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, overlay);
    });
    ExpectDiscCoverage(CoverageOf(overlayOnly, Green));

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, overlay);
        f.renderer.DrawMesh(f.backQuad, Identity, f.opaqueRed, RenderState::Opaque());
    });
    EXPECT_EQ(CoverageOf(frame, Red).count, Width * Height);
}

TEST(SoftwareTextShaderTest, ColourIsAlbedoTimesVertexColour)
{
    Fixture f;
    ASSERT_TRUE(f.Initialize());
    // Yellow albedo times a cyan vertex colour is green
    f.text->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f);
    IMesh *cyanQuad = f.renderer.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    ASSERT_NE(cyanQuad, nullptr);

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(cyanQuad, Identity, f.text, RenderState::Transparent());
    });
    ExpectDiscCoverage(CoverageOf(frame, Green));
    ExpectDisc(frame, Green, Black);
}

TEST(SoftwareTextShaderTest, FullyTransparentTextDrawsNothing)
{
    // OpenGL discards below alpha 0.01; the alpha test does too
    Fixture f;
    ASSERT_TRUE(f.Initialize());
    f.text->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 0.0f);

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, f.text, RenderState::Transparent());
        f.renderer.DrawMesh(f.backQuad, Identity, f.opaqueRed, RenderState::Opaque());
    });
    EXPECT_EQ(CoverageOf(frame, Red).count, Width * Height) << "no colour and no depth written";
}

TEST(SoftwareTextShaderTest, TextWithoutAnAtlasDrawsNothing)
{
    Fixture f;
    ASSERT_TRUE(f.Initialize());
    IMaterial *noAtlas = f.renderer.CreateMaterial(f.renderer.GetStandardTextShader());
    ASSERT_NE(noAtlas, nullptr);
    noAtlas->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);

    const Frame frame = f.Render([&] {
        f.renderer.DrawMesh(f.textQuad, Identity, noAtlas, RenderState::Transparent());
    });
    EXPECT_EQ(CoverageOf(frame, Black).count, Width * Height);
}

// ============================================================================
// Resource lifetime against the frame in flight
// ============================================================================

TEST(SoftwareResourceLifetimeTest, DestroyingWhileAFrameIsInFlightWaitsForIt)
{
    // EndFrame hands the frame to the render thread, which reads the mesh and samples the texture until
    // it finishes. Each destroy waits for it first, so the frame comes out whole. The frame is made long
    // (many full-screen draws) so it is very likely still rasterizing when the destroys run; without the
    // wait, freed memory would be read (a sanitizer would report it even when the pixels survive).
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    constexpr std::uint8_t greenTexel[4] = {0, 255, 0, 255};
    ITexture *texture = renderer.CreateTexture(greenTexel, 1, 1, 4);
    ASSERT_NE(texture, nullptr);
    IMaterial *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader(), texture);
    ASSERT_NE(material, nullptr);
    IMesh *quad = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    renderer.BeginFrame();
    renderer.SetViewProjection(Identity, Identity);
    for (int i = 0; i < 200; ++i)
    {
        renderer.DrawMesh(quad, Identity, material);
    }
    renderer.EndFrame();
    // No Present: the frame may still be rasterizing
    renderer.DestroyMesh(quad);
    renderer.DestroyMaterial(material);
    renderer.DestroyTexture(texture);

    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
    renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);
    EXPECT_EQ(CoverageOf(frame, Green).count, Width * Height);

    renderer.Shutdown();
}

// ============================================================================
// Material uniforms are taken when the draw is recorded, as on OpenGL
// ============================================================================

TEST(SoftwareMaterialSnapshotTest, AChangeAfterDrawMeshDoesNotAffectThatDraw)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    IMaterial *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    IMesh *quad = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    renderer.BeginFrame();
    renderer.SetViewProjection(Identity, Identity);
    renderer.DrawMesh(quad, Identity, material);
    // Changed before the frame is submitted, and again while it may be rasterizing
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    renderer.EndFrame();
    material->SetColor("uAlbedo", 0.0f, 0.0f, 1.0f, 1.0f);
    renderer.Present();

    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
    renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);
    EXPECT_EQ(CoverageOf(frame, Red).count, Width * Height) << "the colour the material had at DrawMesh";

    renderer.Shutdown();
}

TEST(SoftwareMaterialSnapshotTest, TwoDrawsOfOneMaterialKeepTheColourEachWasDrawnWith)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    IMaterial *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    IMesh *leftHalf = renderer.CreateMesh(Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f));
    IMesh *rightHalf = renderer.CreateMesh(Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(leftHalf, nullptr);
    ASSERT_NE(rightHalf, nullptr);

    renderer.BeginFrame();
    renderer.SetViewProjection(Identity, Identity);
    material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    renderer.DrawMesh(leftHalf, Identity, material);
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    renderer.DrawMesh(rightHalf, Identity, material);
    renderer.EndFrame();
    renderer.Present();

    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
    renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);
    const Coverage red = CoverageOf(frame, Red);
    const Coverage green = CoverageOf(frame, Green);
    EXPECT_EQ(red.count, Width * Height / 2);
    EXPECT_EQ(red.maxX, Width / 2 - 1);
    EXPECT_EQ(green.count, Width * Height / 2);
    EXPECT_EQ(green.minX, Width / 2);

    renderer.Shutdown();
}

// ============================================================================
// Index ranges (submeshes), vertex colour and the alpha cutoff (#3 P2)
// ============================================================================

namespace
{
    constexpr Rgb Blue{0, 0, 255};

    /// One mesh holding two quads: the left half of the screen (indices 0..5), then the right half (6..11)
    MeshData TwoHalves()
    {
        MeshData data = Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
        const MeshData right = Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f);
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.insert(data.vertices.end(), right.vertices.begin(), right.vertices.end());
        for (const std::uint32_t index : right.indices)
        {
            data.indices.push_back(base + index);
        }
        return data;
    }

    template <typename Draws>
    Frame RenderFrame(SoftwareRenderer &renderer, const Draws &draws)
    {
        renderer.BeginFrame();
        renderer.SetViewProjection(Identity, Identity);
        draws();
        renderer.EndFrame();
        renderer.Present();
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Width) * Height * 4), Width, Height};
        renderer.ReadFramebuffer(frame.rgba.data(), Width, Height);
        return frame;
    }
}

TEST(SoftwareIndexRangeTest, DrawsOnlyTheTrianglesInTheRange)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    IMaterial *red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    IMaterial *green = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    IMesh *mesh = renderer.CreateMesh(TwoHalves());
    ASSERT_NE(red, nullptr);
    ASSERT_NE(green, nullptr);
    ASSERT_NE(mesh, nullptr);
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    green->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);

    // Only the right half
    Frame frame = RenderFrame(renderer, [&] { renderer.DrawMesh(mesh, Identity, red, RenderState{}, {6, 6}); });
    Coverage c = CoverageOf(frame, Red);
    EXPECT_EQ(c.count, Width * Height / 2);
    EXPECT_EQ(c.minX, Width / 2);
    EXPECT_EQ(CoverageOf(frame, Black).count, Width * Height / 2) << "the left half is untouched";

    // Each half with its own material: two submeshes, two colours
    frame = RenderFrame(renderer, [&]
    {
        renderer.DrawMesh(mesh, Identity, red, RenderState{}, {0, 6});
        renderer.DrawMesh(mesh, Identity, green, RenderState{}, {6, 6});
    });
    c = CoverageOf(frame, Red);
    EXPECT_EQ(c.count, Width * Height / 2);
    EXPECT_EQ(c.maxX, Width / 2 - 1);
    EXPECT_EQ(CoverageOf(frame, Green).minX, Width / 2);

    // An empty range, or one starting past the end, draws nothing; one running past the end draws what is inside
    frame = RenderFrame(renderer, [&]
    {
        renderer.DrawMesh(mesh, Identity, red, RenderState{}, {0, 0});
        renderer.DrawMesh(mesh, Identity, red, RenderState{}, {12, 6});
    });
    EXPECT_EQ(CoverageOf(frame, Black).count, Width * Height);
    frame = RenderFrame(renderer, [&] { renderer.DrawMesh(mesh, Identity, green, RenderState{}, {6, 600}); });
    EXPECT_EQ(CoverageOf(frame, Green).count, Width * Height / 2);

    renderer.Shutdown();
}

TEST(SoftwareVertexColourTest, UnlitAndLitMultiplyByTheVertexColour)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    // Green vertices under a white material: green
    IMesh *greenVertices = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f));
    IMaterial *unlit = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(greenVertices, nullptr);
    ASSERT_NE(unlit, nullptr);
    Frame frame = RenderFrame(renderer, [&] { renderer.DrawMesh(greenVertices, Identity, unlit); });
    EXPECT_EQ(CoverageOf(frame, Green).count, Width * Height);

    // ...and times a yellow material: still green (yellow x green)
    unlit->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f);
    frame = RenderFrame(renderer, [&] { renderer.DrawMesh(greenVertices, Identity, unlit); });
    EXPECT_EQ(CoverageOf(frame, Green).count, Width * Height);

    // Lit, with only ambient light (white, full strength): the albedo times the vertex colour
    Renderer::Common::SceneLightingData lighting;
    lighting.ambientColor = N2Engine::Math::Vector3(1.0f, 1.0f, 1.0f);
    renderer.UpdateSceneLighting(lighting, N2Engine::Math::Vector3(0.0f, 0.0f, 5.0f));
    IMaterial *lit = renderer.CreateMaterial(renderer.GetStandardLitShader());
    ASSERT_NE(lit, nullptr);
    frame = RenderFrame(renderer, [&] { renderer.DrawMesh(greenVertices, Identity, lit); });
    const Rgb centre = frame.At(Width / 2, Height / 2);
    EXPECT_EQ(centre.r, 0);
    EXPECT_GT(centre.g, 200);
    EXPECT_EQ(centre.b, 0);

    renderer.Shutdown();
}

TEST(SoftwareAlphaCutoffTest, PixelsBelowTheCutoffAreSkipped)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, Width, Height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    // Left texel transparent blue, right texel opaque green
    constexpr std::uint8_t texels[8] = {0, 0, 255, 0, 0, 255, 0, 255};
    TextureOptions nearest;
    nearest.filter = Renderer::Common::TextureFilter::Nearest;
    ITexture *texture = renderer.CreateTexture(texels, 2, 1, 4, nearest);
    ASSERT_NE(texture, nullptr);
    IMaterial *cutout = renderer.CreateMaterial(renderer.GetStandardUnlitShader(), texture);
    IMaterial *red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    IMesh *front = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMesh *back = renderer.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    ASSERT_NE(cutout, nullptr);
    ASSERT_NE(red, nullptr);
    ASSERT_NE(front, nullptr);
    ASSERT_NE(back, nullptr);
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);

    // No cutoff (the default, 0): every pixel is written, the transparent half as its colour (no blending)
    Frame frame = RenderFrame(renderer, [&] { renderer.DrawMesh(front, Identity, cutout); });
    EXPECT_EQ(CoverageOf(frame, Green).count, Width * Height / 2);
    EXPECT_EQ(CoverageOf(frame, Blue).count, Width * Height / 2);

    // Cutoff 0.5: the transparent half is skipped, colour and depth, so the red quad behind it shows there
    cutout->SetFloat("uAlphaCutoff", 0.5f);
    frame = RenderFrame(renderer, [&]
    {
        renderer.DrawMesh(front, Identity, cutout);
        renderer.DrawMesh(back, Identity, red);
    });
    const Coverage green = CoverageOf(frame, Green);
    const Coverage redBehind = CoverageOf(frame, Red);
    EXPECT_EQ(green.count, Width * Height / 2);
    EXPECT_EQ(green.minX, Width / 2);
    EXPECT_EQ(redBehind.count, Width * Height / 2) << "seen through the hole";
    EXPECT_EQ(redBehind.maxX, Width / 2 - 1);
    EXPECT_EQ(CoverageOf(frame, Blue).count, 0);

    // A flat colour below the cutoff draws nothing at all
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 0.25f);
    red->SetFloat("uAlphaCutoff", 0.5f);
    frame = RenderFrame(renderer, [&] { renderer.DrawMesh(back, Identity, red); });
    EXPECT_EQ(CoverageOf(frame, Black).count, Width * Height);

    renderer.Shutdown();
}
