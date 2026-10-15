#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The Vulkan backend's unlit drawing (#43, stage B), at renderer level: real frames drawn with the built-in unlit shader
// on the offscreen target and read back, on a Vulkan device (Mesa lavapipe in CI), with validation on (zero errors and
// warnings, nothing leaked at Shutdown). They mirror the software renderer's golden tests (SoftwareRendererGoldenTests.cpp)
// where the behaviour is shared, and say so where it is not:
//   - ReadFramebuffer is bottom row first, as the software renderer's;
//   - clip space is OpenGL's (z from -1 to 1, y up) after the vertex shader's z remap and the negative-height viewport;
//   - depth: LESS, with the draw's depth test and depth write; cull: counter-clockwise front faces, as GL;
//   - IndexRange: only the triangles in the range, and a range that is not inside the index buffer draws NOTHING (as
//     OpenGL; the SOFTWARE renderer clips such a range to the mesh and draws what is inside: a difference on purpose);
//   - a mesh with vertices and no indices draws nothing;
//   - the material is a snapshot taken at DrawMesh (a change afterwards does not affect that draw).
// The draws are exact: a quad on a pixel boundary covers exactly its pixels, so counts and colours are compared exactly.

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::AuxTexture;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::RenderObject;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Vulkan::VulkanRenderer;

namespace
{
    TextureOptions Nearest()
    {
        TextureOptions options;
        options.filter = TextureFilter::Nearest;
        options.wrap = TextureWrap::ClampToEdge;
        options.mipmaps = false;
        return options;
    }

    /// A triangle across most of the frame; counter-clockwise (front-facing in GL) or its mirror
    MeshData Triangle(const bool counterClockwise)
    {
        MeshData data;
        data.vertices = {
            Vertex{{-0.8f, -0.8f, 0.0f}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{0.8f, -0.8f, 0.0f}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{0.0f, 0.8f, 0.0f}, {0, 0, 1}, {0.5f, 1}, {1, 1, 1, 1}},
        };
        data.indices = counterClockwise ? std::vector<std::uint32_t>{0, 1, 2} : std::vector<std::uint32_t>{0, 2, 1};
        return data;
    }

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

    /// A triangle that fills a good part of the frame (its interior is empty when drawn as an outline)
    MeshData BigTriangle()
    {
        MeshData data;
        data.vertices = {
            Vertex{{-0.9f, -0.9f, 0.0f}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{0.9f, -0.9f, 0.0f}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{0.0f, 0.9f, 0.0f}, {0, 0, 1}, {0.5f, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2};
        return data;
    }

    /// A mesh and a material that are not the renderer's, only compared and never dereferenced
    struct FakeMesh final : Renderer::Common::IMesh
    {
        [[nodiscard]] bool IsValid() const override { return true; }
        [[nodiscard]] std::uint32_t GetIndexCount() const override { return 6; }
        [[nodiscard]] std::uint32_t GetVertexCount() const override { return 4; }
    };

    struct FakeMaterial final : Renderer::Common::IMaterial
    {
        void SetInt(const std::string&, int) override {}
        void SetFloat(const std::string&, float) override {}
        void SetVec2(const std::string&, float, float) override {}
        void SetVec2(const std::string&, N2Engine::Math::Vector2&) override {}
        void SetVec3(const std::string&, float, float, float) override {}
        void SetVec3(const std::string&, N2Engine::Math::Vector3&) override {}
        void SetVec4(const std::string&, float, float, float, float) override {}
        void SetVec4(const std::string&, N2Engine::Math::Vector4&) override {}
        void SetColor(const std::string&, float, float, float, float) override {}
        void SetTexture(Renderer::Common::ITexture*) override {}
        [[nodiscard]] Renderer::Common::IShader* GetShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::ITexture* GetTexture() const override { return nullptr; }
        [[nodiscard]] bool IsValid() const override { return true; }
    };
}

// ============================================================================
// Frame layout and clip space
// ============================================================================

TEST_F(VulkanDrawTest, ReadFramebufferIsRgbaWithTheBottomRowFirst)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* bottomHalf = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 0.0f, 0.0f));
    ASSERT_NE(red, nullptr);
    ASSERT_NE(bottomHalf, nullptr);

    const Frame frame = RenderFrame([&] { R().DrawMesh(bottomHalf, Identity, red); });

    // NDC y -1..0 is the bottom half of the image, which is the first half of the buffer
    const Coverage c = CoverageOf(frame, kRed);
    EXPECT_EQ(c.count, Size * Size / 2);
    EXPECT_EQ(c.minY, 0);
    EXPECT_EQ(c.maxY, Size / 2 - 1);
    EXPECT_EQ(frame.rgba[0], 255) << "R first";
    EXPECT_EQ(frame.rgba[1], 0);
    EXPECT_EQ(frame.rgba[2], 0) << "then G and B";
    EXPECT_EQ(frame.AlphaAt(0, 0), 255);
    EXPECT_EQ(CoverageOf(frame, kBlack).count, Size * Size / 2) << "the top half is untouched";
}

TEST_F(VulkanDrawTest, ClipSpaceIsOpenGlsTheNearPlaneAtMinusOneTheFarPlaneAtOneAndNothingBeyond)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* atNear = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, -1.0f));
    IMesh* atFar = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 1.0f));
    IMesh* nearlyFar = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.999f));
    IMesh* beforeNear = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, -1.01f));
    IMesh* beyondFar = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 1.01f));
    ASSERT_NE(beyondFar, nullptr);
    RenderState noDepth;
    noDepth.depthTest = false;
    noDepth.depthWrite = false;

    // Exactly on the near plane: drawn
    Frame frame = RenderFrame([&] { R().DrawMesh(atNear, Identity, red); });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "z = -1 is on the near plane and inside the frustum";

    // Exactly on the far plane: drawn (the depth test is off, as the buffer is cleared to the far plane's 1.0 and LESS
    // keeps nothing that far, in GL too)
    frame = RenderFrame([&] { R().DrawMesh(atFar, Identity, green, noDepth); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "z = +1 is on the far plane and inside the frustum";

    // Just inside the far plane: drawn with the depth test on
    frame = RenderFrame([&] { R().DrawMesh(nearlyFar, Identity, green); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // Beyond either plane: clipped, whatever the depth state
    frame = RenderFrame([&]
    {
        R().DrawMesh(beforeNear, Identity, red, noDepth);
        R().DrawMesh(beyondFar, Identity, green, noDepth);
        R().DrawMesh(beforeNear, Identity, red);
        R().DrawMesh(beyondFar, Identity, green);
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "nothing outside the clip volume is drawn";
}

// ============================================================================
// Depth, depth write and cull (the pipeline state of each draw)
// ============================================================================

TEST_F(VulkanDrawTest, TheNearerQuadWinsWithTheDepthTestOnInEitherOrderAndTheLaterOneWinsWithItOff)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* nearQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, -0.5f)); // nearer in GL (smaller NDC z)
    IMesh* farQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    ASSERT_NE(farQuad, nullptr);

    // Depth test on (the default state): the nearer quad wins whichever is drawn first
    Frame frame = RenderFrame([&]
    {
        R().DrawMesh(farQuad, Identity, green);
        R().DrawMesh(nearQuad, Identity, red);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "far first, then near";
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "near first, then far: LESS keeps the near one";

    // Depth test off: the later draw wins, near or far
    RenderState noTest;
    noTest.depthTest = false;
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, noTest);
        R().DrawMesh(farQuad, Identity, green, noTest);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "later wins without a depth test";
    frame = RenderFrame([&]
    {
        R().DrawMesh(farQuad, Identity, green, noTest);
        R().DrawMesh(nearQuad, Identity, red, noTest);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size);

    // Depth test on but depth write off: the near quad leaves no depth, so a later farther draw shows where it would
    // otherwise be hidden
    RenderState noWrite;
    noWrite.depthWrite = false;
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, noWrite);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "no depth was written by the near quad";
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red); // writes depth
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "the contrast: with depth written the far quad stays hidden";

    // Nothing writes depth while the test is off (GL): a later draw that tests sees an untouched depth buffer
    RenderState testOffWriteOn;
    testOffWriteOn.depthTest = false;
    testOffWriteOn.depthWrite = true;
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, testOffWriteOn);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "depthWrite means nothing with the depth test off";
}

TEST_F(VulkanDrawTest, ACounterClockwiseTriangleShowsWithBackAndNoCullingAndAClockwiseOneTheOtherWayRound)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* ccw = MakeMesh(Triangle(true));
    IMesh* cw = MakeMesh(Triangle(false));
    ASSERT_NE(cw, nullptr);

    struct Case
    {
        CullMode cull;
        bool ccwVisible;
        bool cwVisible;
        const char* name;
    };
    const Case cases[] = {
        {CullMode::Back, true, false, "Back culls the back (clockwise) face"},
        {CullMode::Front, false, true, "Front culls the front (counter-clockwise) face"},
        {CullMode::None, true, true, "None culls nothing"},
    };
    for (const Case& test : cases)
    {
        RenderState state;
        state.cull = test.cull;
        const Frame ccwFrame = RenderFrame([&] { R().DrawMesh(ccw, Identity, red, state); });
        const Frame cwFrame = RenderFrame([&] { R().DrawMesh(cw, Identity, red, state); });
        if (test.ccwVisible)
        {
            EXPECT_GT(ccwFrame.Count(kRed), 1000) << test.name << ": counter-clockwise";
            EXPECT_EQ(ccwFrame.At(Size / 2, 20), kRed) << test.name;
        }
        else
        {
            EXPECT_EQ(ccwFrame.Count(kRed), 0) << test.name << ": counter-clockwise";
        }
        if (test.cwVisible)
        {
            EXPECT_GT(cwFrame.Count(kRed), 1000) << test.name << ": clockwise";
        }
        else
        {
            EXPECT_EQ(cwFrame.Count(kRed), 0) << test.name << ": clockwise";
        }
    }
}

TEST_F(VulkanDrawTest, TheDefaultStateCullsBackFacesAndDepthTestsAndWritesAsTheOpaqueQueueDoes)
{
    // RenderState{} is RenderState::Opaque(): the overload without a state draws with it
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* ccw = MakeMesh(Triangle(true));
    IMesh* cw = MakeMesh(Triangle(false));
    ASSERT_NE(cw, nullptr);
    const Frame front = RenderFrame([&] { R().DrawMesh(ccw, Identity, red); });
    const Frame back = RenderFrame([&] { R().DrawMesh(cw, Identity, red); });
    EXPECT_GT(front.Count(kRed), 1000);
    EXPECT_EQ(back.Count(kRed), 0);
}

TEST_F(VulkanDrawTest, ABlendedDrawIsDrawnOpaqueUntilStageD)
{
    // Blending is stage D of #43: blend and additive are accepted, drawn as opaque (and logged once)
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    RenderState additive = RenderState::Transparent();
    additive.additive = true;
    const Frame blended = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent()); });
    EXPECT_EQ(blended.Count(kRed), Size * Size);
    const Frame added = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, additive); });
    EXPECT_EQ(added.Count(kRed), Size * Size);
}

// ============================================================================
// Index ranges (submeshes) and meshes without indices
// ============================================================================

TEST_F(VulkanDrawTest, IndexRangeDrawsOnlyTheTrianglesInTheRange)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* mesh = MakeMesh(TwoHalves());
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->GetIndexCount(), 12u);

    // Only the right half
    Frame frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{6, 6}); });
    Coverage c = CoverageOf(frame, kRed);
    EXPECT_EQ(c.count, Size * Size / 2);
    EXPECT_EQ(c.minX, Size / 2);
    EXPECT_EQ(CoverageOf(frame, kBlack).count, Size * Size / 2) << "the left half is untouched";

    // Each half with its own material: two submeshes, two colours
    frame = RenderFrame([&]
    {
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 6});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{6, 6});
    });
    c = CoverageOf(frame, kRed);
    EXPECT_EQ(c.count, Size * Size / 2);
    EXPECT_EQ(c.maxX, Size / 2 - 1);
    EXPECT_EQ(CoverageOf(frame, kGreen).minX, Size / 2);
    EXPECT_EQ(CoverageOf(frame, kGreen).count, Size * Size / 2);

    // The whole mesh, as a range and as no range
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 12}); });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, green); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // An empty range, or one starting at or past the end, draws nothing
    frame = RenderFrame([&]
    {
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 0});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{12, 6});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{12, 0});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{4000000000u, 6});
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);

    // A range that runs past the end draws NOTHING: the OpenGL renderer's rule, which Vulkan follows. (The software
    // renderer clips such a range to the mesh and draws what is inside it; its SoftwareIndexRangeTest says so.)
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{6, 600}); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "first + count is past the index count";
    frame = RenderFrame([&]
    {
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{0, 13});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{7, 6});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{4294967295u, 2});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{1, 4294967295u});
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "including a sum that overflows 32 bits";

    // The last triangle of the buffer is still inside it
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{9, 3}); });
    EXPECT_GT(frame.Count(kGreen), 0);
    EXPECT_LT(frame.Count(kGreen), Size * Size / 2);
}

TEST_F(VulkanDrawTest, AMeshWithVerticesAndNoIndicesDrawsNothingAndBindsNoIndexBuffer)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    MeshData noIndices = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
    noIndices.indices.clear();
    IMesh* mesh = MakeMesh(noIndices);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetIndexCount(), 0u);
    EXPECT_EQ(mesh->GetVertexCount(), 4u);

    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(mesh, Identity, red);
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 3});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 0});
        std::vector<RenderObject> objects(1);
        objects[0].mesh = mesh;
        objects[0].material = red;
        std::copy(std::begin(Identity), std::end(Identity), objects[0].transform.model);
        R().DrawObjects(objects);
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
}

// ============================================================================
// Colour: albedo x texture x vertex colour, the alpha cutoff
// ============================================================================

TEST_F(VulkanDrawTest, TheColourIsTheAlbedoTimesTheVertexColour)
{
    // Green vertices under a white material: green
    IMesh* greenVertices = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f));
    IMaterial* material = R().CreateMaterial(_unlit);
    ASSERT_NE(greenVertices, nullptr);
    ASSERT_NE(material, nullptr);
    Frame frame = RenderFrame([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "the default albedo is white";

    // ...and times a yellow material: still green (yellow x green)
    material->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f);
    frame = RenderFrame([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // ...and times a red one: black
    material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    frame = RenderFrame([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
}

TEST_F(VulkanDrawTest, ANearestTextureIsSampledTimesTheAlbedoAndTheVertexColourWithExactColours)
{
    // 2 x 1 texels, red then blue, on a quad that spans the frame: the left half is the first texel
    const std::uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    ITexture* texture = R().CreateTexture(texels, 2, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);

    Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, material); });
    EXPECT_EQ(frame.Count(kRed), Size * Size / 2);
    EXPECT_EQ(frame.Count(kBlue), Size * Size / 2);
    EXPECT_EQ(CoverageOf(frame, kRed).maxX, Size / 2 - 1) << "u = 0 is the left";
    EXPECT_EQ(CoverageOf(frame, kBlue).minX, Size / 2);

    // The texture is upright: a 1 x 2 texture, red in its first row (v = 0, the bottom) and blue in its second
    const std::uint8_t column[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    ITexture* upright = R().CreateTexture(column, 1, 2, 4, Nearest());
    IMaterial* uprightMaterial = R().CreateMaterial(_unlit, upright);
    ASSERT_NE(uprightMaterial, nullptr);
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, uprightMaterial); });
    EXPECT_EQ(frame.At(Size / 2, 8), kRed) << "v = 0 is the bottom of the quad (the first row of the texture)";
    EXPECT_EQ(frame.At(Size / 2, Size - 8), kBlue) << "v = 1 is the top";

    // Texture x albedo x vertex colour: a yellow texel, a cyan albedo and white vertices are green
    const std::uint8_t yellowTexel[4] = {255, 255, 0, 255};
    ITexture* yellow = R().CreateTexture(yellowTexel, 1, 1, 4, Nearest());
    IMaterial* cyanOnYellow = R().CreateMaterial(_unlit, yellow);
    ASSERT_NE(cyanOnYellow, nullptr);
    cyanOnYellow->SetColor("uAlbedo", 0.0f, 1.0f, 1.0f, 1.0f);
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, cyanOnYellow); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // ...and a vertex colour on top: yellow texel x white albedo x cyan vertices is green too
    IMesh* cyanVertices = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    IMaterial* plainYellow = R().CreateMaterial(_unlit, yellow);
    ASSERT_NE(plainYellow, nullptr);
    frame = RenderFrame([&] { R().DrawMesh(cyanVertices, Identity, plainYellow); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // uHasTexture 0 ignores the texture
    plainYellow->SetInt("uHasTexture", 0);
    frame = RenderFrame([&] { R().DrawMesh(cyanVertices, Identity, plainYellow); });
    EXPECT_EQ(frame.Count(Rgb{0, 255, 255}), Size * Size) << "white albedo x cyan vertices, no texture";
}

TEST_F(VulkanDrawTest, PixelsBelowTheAlphaCutoffAreSkippedColourAndDepth)
{
    // Left texel transparent blue, right texel opaque green
    const std::uint8_t texels[8] = {0, 0, 255, 0, 0, 255, 0, 255};
    ITexture* texture = R().CreateTexture(texels, 2, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* cutout = R().CreateMaterial(_unlit, texture);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* front = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMesh* back = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    ASSERT_NE(cutout, nullptr);
    ASSERT_NE(red, nullptr);
    ASSERT_NE(front, nullptr);
    ASSERT_NE(back, nullptr);

    // No cutoff (the default, 0): every pixel is written, the transparent half as its colour (no blending)
    Frame frame = RenderFrame([&] { R().DrawMesh(front, Identity, cutout); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size / 2);
    EXPECT_EQ(frame.Count(kBlue), Size * Size / 2);

    // Cutoff 0.5: the transparent half is skipped, colour and depth, so the red quad behind it shows there
    cutout->SetFloat("uAlphaCutoff", 0.5f);
    frame = RenderFrame([&]
    {
        R().DrawMesh(front, Identity, cutout);
        R().DrawMesh(back, Identity, red);
    });
    const Coverage green = CoverageOf(frame, kGreen);
    const Coverage redBehind = CoverageOf(frame, kRed);
    EXPECT_EQ(green.count, Size * Size / 2);
    EXPECT_EQ(green.minX, Size / 2);
    EXPECT_EQ(redBehind.count, Size * Size / 2) << "seen through the hole";
    EXPECT_EQ(redBehind.maxX, Size / 2 - 1);
    EXPECT_EQ(frame.Count(kBlue), 0);

    // A flat colour below the cutoff draws nothing at all
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 0.25f);
    red->SetFloat("uAlphaCutoff", 0.5f);
    frame = RenderFrame([&] { R().DrawMesh(back, Identity, red); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
}

// ============================================================================
// Material values are taken when the draw is recorded, as on OpenGL
// ============================================================================

TEST_F(VulkanDrawTest, AChangeAfterDrawMeshDoesNotAffectThatDraw)
{
    IMaterial* material = R().CreateMaterial(_unlit);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);

    material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, material);
    // Changed before the frame is submitted, and again while it may be running
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    R().EndFrame();
    material->SetColor("uAlbedo", 0.0f, 0.0f, 1.0f, 1.0f);
    const Frame frame = ReadFrame();
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "the colour the material had at DrawMesh";
}

TEST_F(VulkanDrawTest, TwoDrawsOfOneMaterialKeepTheColourEachWasDrawnWith)
{
    IMaterial* material = R().CreateMaterial(_unlit);
    IMesh* leftHalf = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f));
    IMesh* rightHalf = MakeMesh(Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(leftHalf, nullptr);
    ASSERT_NE(rightHalf, nullptr);

    const Frame frame = RenderFrame([&]
    {
        material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
        R().DrawMesh(leftHalf, Identity, material);
        material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
        R().DrawMesh(rightHalf, Identity, material);
    });
    const Coverage red = CoverageOf(frame, kRed);
    const Coverage green = CoverageOf(frame, kGreen);
    EXPECT_EQ(red.count, Size * Size / 2);
    EXPECT_EQ(red.maxX, Size / 2 - 1);
    EXPECT_EQ(green.count, Size * Size / 2);
    EXPECT_EQ(green.minX, Size / 2);
}

TEST_F(VulkanDrawTest, ATextureChangedAfterDrawMeshDoesNotAffectThatDraw)
{
    const std::uint8_t redTexel[4] = {255, 0, 0, 255};
    const std::uint8_t greenTexel[4] = {0, 255, 0, 255};
    ITexture* red = R().CreateTexture(redTexel, 1, 1, 4, Nearest());
    ITexture* green = R().CreateTexture(greenTexel, 1, 1, 4, Nearest());
    IMaterial* material = R().CreateMaterial(_unlit, red);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);
    ASSERT_NE(green, nullptr);

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, material);
    material->SetTexture(green);
    EXPECT_EQ(material->GetTexture(), green);
    R().EndFrame();
    EXPECT_EQ(ReadFrame().Count(kRed), Size * Size);
}

// ============================================================================
// The matrices: row-major as the engine passes them, view and projection as last set
// ============================================================================

TEST_F(VulkanDrawTest, TheModelMatrixIsRowMajorWithItsTranslationInTheLastColumn)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* centred = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(centred, nullptr);

    // Translated right by 0.5: the quad's x range -0.5..0.5 becomes 0..1, pixels 32..63 across and 16..47 up
    float model[16];
    Translation(model, 0.5f, 0.0f, 0.0f);
    Frame frame = RenderFrame([&] { R().DrawMesh(centred, model, red); });
    Coverage c = CoverageOf(frame, kRed);
    EXPECT_EQ(c.count, 32 * 32);
    EXPECT_EQ(c.minX, 32);
    EXPECT_EQ(c.maxX, 63);
    EXPECT_EQ(c.minY, 16);
    EXPECT_EQ(c.maxY, 47);

    // A quarter turn counter-clockwise (x' = -y, y' = x) takes a quad in the upper right to the upper left; transposed
    // it would go to the lower right
    IMesh* upperRight = MakeMesh(Quad(0.2f, 0.2f, 0.8f, 0.8f, 0.0f));
    ASSERT_NE(upperRight, nullptr);
    const float quarterTurn[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    frame = RenderFrame([&] { R().DrawMesh(upperRight, quarterTurn, red); });
    EXPECT_EQ(frame.At(16, 48), kRed) << "the upper left";
    EXPECT_EQ(frame.At(48, 48), kBlack);
    EXPECT_EQ(frame.At(48, 16), kBlack) << "not the lower right";
    EXPECT_EQ(frame.At(16, 16), kBlack);
}

TEST_F(VulkanDrawTest, ASetViewProjectionInTheMiddleOfAFrameAffectsOnlyTheDrawsAfterIt)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(0.0f, -0.5f, 0.5f, 0.5f, 0.0f)); // x 0..0.5: pixels 32..47
    ASSERT_NE(quad, nullptr);

    float view[16];
    Translation(view, -0.75f, 0.0f, 0.0f); // x -0.75..-0.25: pixels 8..23
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, red);
    R().SetViewProjection(view, Identity);
    R().DrawMesh(quad, Identity, green);
    R().SetViewProjection(nullptr, nullptr); // ignored: the last values stay
    R().DrawMesh(quad, Identity, green);
    R().EndFrame();
    const Frame frame = ReadFrame();
    const Coverage redCoverage = CoverageOf(frame, kRed);
    const Coverage greenCoverage = CoverageOf(frame, kGreen);
    EXPECT_EQ(redCoverage.minX, 32);
    EXPECT_EQ(redCoverage.maxX, 47);
    EXPECT_EQ(redCoverage.count, 16 * 32);
    EXPECT_EQ(greenCoverage.minX, 8);
    EXPECT_EQ(greenCoverage.maxX, 23);
    EXPECT_EQ(greenCoverage.count, 16 * 32);
}

// ============================================================================
// DrawObjects
// ============================================================================

TEST_F(VulkanDrawTest, DrawObjectsIsTheSameDrawMeshCallsInOrderWithEachObjectsOwnState)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMesh* small = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    IMesh* backdrop = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    IMesh* ccw = MakeMesh(Triangle(true));
    ASSERT_NE(ccw, nullptr);

    std::vector<RenderObject> objects(4);
    objects[0].mesh = small;
    objects[0].material = red;
    Translation(objects[0].transform.model, -0.5f, 0.0f, 0.0f);
    objects[1].mesh = small;
    objects[1].material = green;
    Translation(objects[1].transform.model, 0.5f, 0.0f, 0.0f);
    objects[2].mesh = backdrop;
    objects[2].material = blue;
    std::copy(std::begin(Identity), std::end(Identity), objects[2].transform.model);
    objects[3].mesh = ccw;
    objects[3].material = red;
    std::copy(std::begin(Identity), std::end(Identity), objects[3].transform.model);
    objects[3].state.cull = CullMode::Front; // culled: it is counter-clockwise

    const Frame viaObjects = RenderFrame([&] { R().DrawObjects(objects); });
    const Frame viaCalls = RenderFrame([&]
    {
        for (const RenderObject& object : objects)
        {
            R().DrawMesh(object.mesh, object.transform.model, object.material, object.state);
        }
    });
    EXPECT_EQ(viaObjects, viaCalls);
    EXPECT_EQ(viaObjects.Count(kRed), 32 * 32) << "the left square: x -1..0, y -0.5..0.5 (the triangle is culled)";
    EXPECT_EQ(viaObjects.Count(kGreen), 32 * 32) << "the right square";
    EXPECT_EQ(viaObjects.Count(kBlue), Size * Size - 2 * 32 * 32) << "the backdrop, hidden behind the squares by the depth test";
    R().DrawObjects({}); // nothing, outside a frame too
}

// ============================================================================
// What is ignored
// ============================================================================

TEST_F(VulkanDrawTest, DrawsOutsideAFrameAreIgnoredWithoutAMessage)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    std::vector<RenderObject> objects(1);
    objects[0].mesh = quad;
    objects[0].material = red;
    std::copy(std::begin(Identity), std::end(Identity), objects[0].transform.model);

    // Before the first frame, and between frames
    R().DrawMesh(quad, Identity, red);
    R().DrawMesh(quad, Identity, red, RenderState{}, IndexRange{0, 6});
    R().DrawObjects(objects);
    Frame frame = RenderFrame([&] {});
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
    R().DrawMesh(quad, Identity, red);
    R().DrawObjects(objects);
    frame = ReadFrame();
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "draws after EndFrame go nowhere";

    // Inside a frame, the same draws work
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, red); });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
}

TEST_F(VulkanDrawTest, ANullOrForeignMeshOrMaterialOrAnUnsetModelMatrixDrawsNothing)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    // Another renderer's mesh and material, and objects that are not this renderer's at all
    VulkanRenderer& other = NewRenderer();
    ASSERT_TRUE(other.Initialize(nullptr, 8, 8));
    IMesh* foreignMesh = other.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* foreignMaterial = other.CreateMaterial(other.GetStandardUnlitShader());
    ASSERT_NE(foreignMesh, nullptr);
    ASSERT_NE(foreignMaterial, nullptr);
    FakeMesh fakeMesh;
    FakeMaterial fakeMaterial;

    // A destroyed mesh and material
    IMesh* destroyedMesh = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* destroyedMaterial = Colour(0.0f, 0.0f, 1.0f);
    R().DestroyMesh(destroyedMesh);
    R().DestroyMaterial(destroyedMaterial);

    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(nullptr, Identity, red);
        R().DrawMesh(quad, Identity, nullptr);
        R().DrawMesh(quad, nullptr, red);
        R().DrawMesh(quad, nullptr, red, RenderState{}, IndexRange{0, 6});
        R().DrawMesh(foreignMesh, Identity, red);
        R().DrawMesh(quad, Identity, foreignMaterial);
        R().DrawMesh(&fakeMesh, Identity, red);
        R().DrawMesh(quad, Identity, &fakeMaterial);
        R().DrawMesh(destroyedMesh, Identity, red);
        R().DrawMesh(quad, Identity, destroyedMaterial);
        R().DrawMesh(foreignMesh, Identity, foreignMaterial, RenderState{}, IndexRange{0, 6});
        std::vector<RenderObject> objects(2); // null mesh and material
        objects[1].mesh = quad;                // a material-less object
        R().DrawObjects(objects);
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);

    // And what is valid still draws
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), Size * Size);
}

TEST_F(VulkanDrawTest, ATextureDestroyedAfterTheMaterialTookItIsReplacedByTheDefaultTexture)
{
    const std::uint8_t redTexel[4] = {255, 0, 0, 255};
    ITexture* texture = R().CreateTexture(redTexel, 1, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, material); }).Count(kBlack), Size * Size)
        << "red texel x green albedo";
    R().DestroyTexture(texture);
    EXPECT_EQ(material->GetTexture(), texture) << "the material still names it";
    // The destroyed texture is white (the 1x1 default) for the draw: the albedo shows as it is, and nothing crashes
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, material); }).Count(kGreen), Size * Size);
}

// ============================================================================
// Mid-frame destruction and updates
// ============================================================================

TEST_F(VulkanDrawTest, DestroyingWhatADrawUsedBeforeTheFrameEndsKeepsTheDraw)
{
    const std::uint8_t greenTexel[4] = {0, 255, 0, 255};
    for (int frameNumber = 0; frameNumber < 6; ++frameNumber)
    {
        ITexture* texture = R().CreateTexture(greenTexel, 1, 1, 4, Nearest());
        IMaterial* material = R().CreateMaterial(_unlit, texture);
        IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
        ASSERT_NE(material, nullptr);
        ASSERT_NE(quad, nullptr);

        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().DrawMesh(quad, Identity, material);
        R().DestroyMesh(quad);
        R().DestroyMaterial(material);
        R().DestroyTexture(texture);
        R().EndFrame();
        EXPECT_EQ(ReadFrame().Count(kGreen), Size * Size) << "frame " << frameNumber;
    }
    // The retired buffers and images are freed as the frame slots come round
    RenderFrame([] {});
    RenderFrame([] {});
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveAllocationCountForTest(), 0u) << "only the renderer's own allocations are left";
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
}

TEST_F(VulkanDrawTest, UpdateMeshAfterADrawLeavesThatDrawWithTheDataItWasDrawnWith)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* mesh = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f)); // the left half
    ASSERT_NE(mesh, nullptr);
    for (int frameNumber = 0; frameNumber < 6; ++frameNumber)
    {
        const bool leftFirst = frameNumber % 2 == 0;
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().DrawMesh(mesh, Identity, red);
        ASSERT_TRUE(R().UpdateMesh(mesh, leftFirst ? Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f) : Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f)));
        R().DrawMesh(mesh, Identity, red);
        R().EndFrame();
        const Frame frame = ReadFrame();
        // Both halves are red: the first draw used the old data, the second the new
        EXPECT_EQ(frame.Count(kRed), Size * Size) << "frame " << frameNumber;
    }
}

// ============================================================================
// Wireframe
// ============================================================================

TEST_F(VulkanDrawTest, WireframeDrawsTheOutlineOfATriangleAndLeavesItsInteriorTheBackground)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    MeshData big;
    big.vertices = {
        Vertex{{-0.9f, -0.9f, 0.0f}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
        Vertex{{0.9f, -0.9f, 0.0f}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
        Vertex{{0.0f, 0.9f, 0.0f}, {0, 0, 1}, {0.5f, 1}, {1, 1, 1, 1}},
    };
    big.indices = {0, 1, 2};
    IMesh* mesh = MakeMesh(big);
    ASSERT_NE(mesh, nullptr);

    // Filled
    Frame frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red); });
    EXPECT_GT(frame.Count(kRed), 1500);
    EXPECT_EQ(frame.At(Size / 2, 20), kRed);

    R().SetWireframe(true);
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red); });
    R().SetWireframe(false);
    if (!R().IsWireframeSupportedForTest())
    {
        // Without fillModeNonSolid SetWireframe is ignored (one log line) and the triangle is filled
        EXPECT_EQ(frame.At(Size / 2, 20), kRed);
        GTEST_SKIP() << "this device has no fillModeNonSolid: wireframe is ignored, so there is no outline to check";
    }
    EXPECT_EQ(frame.At(Size / 2, 20), kBlack) << "the interior is the background";
    EXPECT_GT(frame.Count(kRed), 50) << "the outline";
    EXPECT_LT(frame.Count(kRed), 700);

    // Back to filled
    frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red); });
    EXPECT_EQ(frame.At(Size / 2, 20), kRed);
}

// ============================================================================
// Materials
// ============================================================================

TEST_F(VulkanDrawTest, AMaterialHasTheOpenGlDefaultsAndKeepsItsTextures)
{
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* plain = R().CreateMaterial(_unlit);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->GetShader(), _unlit);
    EXPECT_TRUE(plain->IsValid());
    EXPECT_EQ(plain->GetTexture(), nullptr);
    for (const AuxTexture which : {AuxTexture::Emissive, AuxTexture::Occlusion, AuxTexture::Normal, AuxTexture::MetallicRoughness})
    {
        EXPECT_EQ(plain->GetAuxTexture(which), nullptr);
    }
    // White, uHasTexture 0, no cutoff
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, plain); }).Count(kWhite), Size * Size);

    const std::uint8_t texel[4] = {255, 0, 0, 255};
    ITexture* texture = R().CreateTexture(texel, 1, 1, 4, Nearest());
    ITexture* other = R().CreateTexture(texel, 1, 1, 4, Nearest());
    IMaterial* textured = R().CreateMaterial(_unlit, texture);
    ASSERT_NE(textured, nullptr);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(textured->GetTexture(), texture);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, textured); }).Count(kRed), Size * Size) << "uHasTexture is 1 with a texture";
    textured->SetTexture(other);
    EXPECT_EQ(textured->GetTexture(), other);
    textured->SetAuxTexture(AuxTexture::Normal, other);
    EXPECT_EQ(textured->GetAuxTexture(AuxTexture::Normal), other);
    EXPECT_EQ(textured->GetAuxTexture(AuxTexture::Emissive), nullptr);
    textured->SetAuxTexture(AuxTexture::Normal, nullptr);
    EXPECT_EQ(textured->GetAuxTexture(AuxTexture::Normal), nullptr);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, textured); }).Count(kRed), Size * Size);

    // The material setters
    N2Engine::Math::Vector4 blue;
    blue.x = 0.0f; // (the members by name: the constructor's argument order is not the point of this test)
    blue.y = 0.0f;
    blue.z = 1.0f;
    blue.w = 1.0f;
    plain->SetVec4("uAlbedo", blue);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, plain); }).Count(kBlue), Size * Size);
    plain->SetVec4("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, plain); }).Count(kGreen), Size * Size);
}

TEST_F(VulkanDrawTest, AnUnknownUniformNameOrTheWrongTypeIsIgnoredAndTheMaterialStillDraws)
{
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* material = R().CreateMaterial(_unlit);
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);

    material->SetFloat("uNoSuchUniform", 1.0f);
    material->SetInt("uNoSuchUniform", 1);
    material->SetVec3("uNoSuchUniform", 1.0f, 2.0f, 3.0f);
    material->SetColor("uMetallic", 1.0f, 1.0f, 1.0f, 1.0f); // a lit-shader uniform: not in the unlit block
    material->SetFloat("uMetallic", 1.0f);
    material->SetInt("uAlbedo", 3);                          // uAlbedo is a vec4
    material->SetVec3("uAlbedo", 1.0f, 0.0f, 0.0f);
    material->SetFloat("uHasTexture", 1.0f);                 // an int
    material->SetInt("uAlphaCutoff", 1);                     // a float
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, material); }).Count(kGreen), Size * Size);
}

TEST_F(VulkanDrawTest, DestroyMaterialIgnoresNullForeignAndAlreadyDestroyedPointersAndShutdownFreesTheRest)
{
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
    IMaterial* a = R().CreateMaterial(_unlit);
    IMaterial* b = R().CreateMaterial(_unlit);
    IMaterial* c = R().CreateMaterial(_unlit);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 3u);

    VulkanRenderer& other = NewRenderer();
    ASSERT_TRUE(other.Initialize(nullptr, 8, 8));
    IMaterial* foreign = other.CreateMaterial(other.GetStandardUnlitShader());
    ASSERT_NE(foreign, nullptr);
    FakeMaterial fake;

    R().DestroyMaterial(nullptr);
    R().DestroyMaterial(foreign);
    R().DestroyMaterial(&fake);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 3u);
    EXPECT_EQ(other.GetLiveMaterialCountForTest(), 1u);

    R().DestroyMaterial(b);
    R().DestroyMaterial(b); // twice
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 2u);
    R().DestroyMaterial(a);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 1u);

    R().Shutdown(); // c is freed with the renderer
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
    other.DestroyMaterial(foreign);
    EXPECT_EQ(other.GetLiveMaterialCountForTest(), 0u);
}

TEST_F(VulkanDrawTest, ADrawIsKeptWhenTheTargetIsResizedBeforeTheNextFrameAndPipelinesSurviveIt)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), 32 * 32);
    const std::uint32_t pipelines = R().GetPipelineCountForTest();
    EXPECT_GE(pipelines, 1u);

    // A frame abandoned by a resize draws nothing and the next one draws at the new size with the same pipelines
    R().BeginFrame();
    R().DrawMesh(quad, Identity, red);
    ASSERT_TRUE(R().SetRenderTargetSize(32, 48));
    R().DrawMesh(quad, Identity, red); // no frame is open any more
    R().EndFrame();

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, red);
    R().EndFrame();
    std::vector<std::uint8_t> pixels(32 * 48 * 4);
    R().ReadFramebuffer(pixels.data(), 32, 48);
    int count = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        count += (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0) ? 1 : 0;
    }
    EXPECT_EQ(count, 16 * 24) << "a quad of half the frame in each direction";
    EXPECT_EQ(R().GetPipelineCountForTest(), pipelines) << "the viewport is dynamic: no new pipeline for a new size";
}

// ============================================================================
// Review fixes: wireframe asked for before Initialize, a failing pipeline, a recycled texture address
// ============================================================================

namespace
{
    /// One red unlit draw of BigTriangle on a renderer of its own, read back
    Frame DrawBigTriangle(VulkanRenderer& renderer)
    {
        IMesh* mesh = renderer.CreateMesh(BigTriangle());
        IMaterial* material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
        EXPECT_NE(mesh, nullptr);
        EXPECT_NE(material, nullptr);
        if (material != nullptr)
        {
            material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
        }
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        renderer.SetViewProjection(Identity, Identity);
        renderer.DrawMesh(mesh, Identity, material);
        renderer.EndFrame();
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4), Size, Size};
        renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
        return frame;
    }
}

TEST_F(VulkanDrawTest, WireframeAskedForBeforeInitializeBehavesLikeAskedForAfterIt)
{
    VulkanRenderer& before = NewRenderer();
    before.SetWireframe(true);
    if (!before.Initialize(nullptr, 8, 8))
    {
        HandleInitFailure(before);
        return;
    }
    ASSERT_TRUE(before.SetRenderTargetSize(Size, Size));
    VulkanRenderer& after = NewRenderer();
    if (!after.Initialize(nullptr, 8, 8))
    {
        HandleInitFailure(after);
        return;
    }
    ASSERT_TRUE(after.SetRenderTargetSize(Size, Size));
    after.SetWireframe(true);

    const Frame beforeFrame = DrawBigTriangle(before);
    const Frame afterFrame = DrawBigTriangle(after);
    EXPECT_TRUE(beforeFrame == afterFrame) << "SetWireframe(true) before Initialize must act as it does after";
    if (before.IsWireframeSupportedForTest())
    {
        // Honoured: the outline, not silently dropped draws and not a filled triangle
        EXPECT_GT(beforeFrame.Count(kRed), 50);
        EXPECT_EQ(beforeFrame.At(Size / 2, 20), kBlack) << "the interior is the background";
    }
    else
    {
        EXPECT_EQ(beforeFrame.At(Size / 2, 20), kRed) << "wireframe is off: the triangle is solid";
    }
}

TEST_F(VulkanDrawTest, WireframeAskedForBeforeInitializeOnADeviceWithoutTheFeatureDrawsSolidAndLogsOnce)
{
    VulkanRenderer& renderer = NewRenderer();
    renderer.SetWireframeUnavailableForTest(true); // the device may well have fillModeNonSolid (lavapipe does)
    renderer.SetWireframe(true);

    const CerrCapture capture;
    if (!renderer.Initialize(nullptr, 8, 8))
    {
        HandleInitFailure(renderer);
        return;
    }
    ASSERT_TRUE(renderer.SetRenderTargetSize(Size, Size));
    EXPECT_FALSE(renderer.IsWireframeSupportedForTest());

    const Frame frame = DrawBigTriangle(renderer);
    EXPECT_EQ(frame.At(Size / 2, 20), kRed) << "the draw is solid, not dropped";
    EXPECT_GT(frame.Count(kRed), 1500);
    renderer.SetWireframe(true); // after Initialize too: still ignored, still the one line
    EXPECT_EQ(DrawBigTriangle(renderer).At(Size / 2, 20), kRed);
    EXPECT_EQ(CountOccurrences(capture.Text(), "wireframe ignored: fillModeNonSolid not supported"), 1)
        << capture.Text();
}

TEST_F(VulkanDrawTest, APipelineThatCannotBeMadeIsRememberedLoggedOncePerKeyAndItsDrawsAreSkipped)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(red, nullptr);
    ASSERT_NE(quad, nullptr);
    const std::string failure = "vkCreateGraphicsPipelines failed";

    R().SetFailPipelineCreationForTest(true);
    {
        const CerrCapture capture;
        const Frame frame = RenderFrame([&]
        {
            for (int i = 0; i < 200; ++i)
            {
                R().DrawMesh(quad, Identity, red);
            }
        });
        EXPECT_EQ(frame.Count(kBlack), Size * Size) << "nothing is drawn with a pipeline that failed";
        EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 1u) << "tried once, then remembered";
        EXPECT_EQ(CountOccurrences(capture.Text(), failure), 1) << capture.Text();

        // Another key fails once too; the first stays quiet
        Renderer::Common::RenderState noDepth;
        noDepth.depthTest = false;
        RenderFrame([&]
        {
            for (int i = 0; i < 50; ++i)
            {
                R().DrawMesh(quad, Identity, red, noDepth);
                R().DrawMesh(quad, Identity, red);
            }
        });
        EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 2u);
        EXPECT_EQ(CountOccurrences(capture.Text(), failure), 2) << capture.Text();
    }

    // The failure is remembered even once creation would work again; a key never tried still draws
    R().SetFailPipelineCreationForTest(false);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red); }).Count(kBlack), Size * Size);
    Renderer::Common::RenderState noWrite;
    noWrite.depthWrite = false;
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red, noWrite); }).Count(kRed), Size * Size);
}

TEST_F(VulkanDrawTest, ATextureCreatedAtTheAddressOfADestroyedOneIsNotDrawnInItsPlace)
{
    const std::uint8_t redTexel[4] = {255, 0, 0, 255};
    const std::uint8_t blueTexel[4] = {0, 0, 255, 255};
    ITexture* a = R().CreateTexture(redTexel, 1, 1, 4, Nearest());
    ASSERT_NE(a, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit);
    ASSERT_NE(material, nullptr);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    material->SetTexture(a);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, material); }).Count(kRed), Size * Size);

    const std::uint64_t oldId = R().GetTextureIdForTest(a);
    EXPECT_NE(oldId, 0u);
    R().DestroyTexture(a);
    EXPECT_EQ(R().GetTextureIdForTest(a), 0u) << "a destroyed texture has no id";

    // Make textures until one lands on the old address (the allocator often hands it straight back); either way every
    // new texture has an id that is not the old one
    std::vector<ITexture*> made;
    ITexture* b = nullptr;
    for (int attempt = 0; attempt < 64 && b != a; ++attempt)
    {
        b = R().CreateTexture(blueTexel, 1, 1, 4, Nearest());
        ASSERT_NE(b, nullptr);
        made.push_back(b);
        EXPECT_NE(R().GetTextureIdForTest(b), oldId);
        EXPECT_NE(R().GetTextureIdForTest(b), 0u);
    }
    if (b != a)
    {
        std::cout << "[ note ] the address was not reused here; only the ids were checked" << std::endl;
    }

    // The material still names the destroyed texture, so it draws the 1x1 white default, not the new texture
    EXPECT_EQ(material->GetTexture(), a);
    const Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, material); });
    EXPECT_EQ(frame.Count(kWhite), Size * Size) << "the default, not the blue texture made later (nor the destroyed red one)";

    // Setting the new texture afterwards draws it
    material->SetTexture(made.back());
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, material); }).Count(kBlue), Size * Size);
    for (ITexture* texture : made)
    {
        R().DestroyTexture(texture);
    }
}
