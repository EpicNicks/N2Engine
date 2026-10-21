#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
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
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// The WebGPU backend's unlit drawing (#155, stage G3a), at renderer level: real frames drawn with the built-in unlit shader on
// the offscreen target and read back, on Dawn over Mesa lavapipe in CI, with the device's error counter checked at teardown
// (zero WebGPU errors, nothing leaked: WebGpuTestFixture.hpp). They mirror the Vulkan backend's tests
// (tests/renderer/vulkan/VulkanUnlitDrawTests.cpp and VulkanPipelineTests.cpp) and assert the SAME structural values, scene by
// scene, so a WebGPU frame is held to what the Vulkan frame is held to (the Vulkan renderer itself is compiled out of the
// webgpu CI job); the strict comparison with the SOFTWARE renderer is in WebGpuRendererGoldenTests.cpp.
//   - ReadFramebuffer is bottom row first, as the software renderer's;
//   - clip space is OpenGL's (z from -1 to 1, y up) after the vertex shader's z remap, with no viewport flip;
//   - depth: LESS, with the draw's depth test and depth write; cull: counter-clockwise front faces, as GL;
//   - IndexRange: only the triangles in the range, and a range that is not inside the index buffer draws NOTHING (as OpenGL and
//     Vulkan; the SOFTWARE renderer clips such a range to the mesh and draws what is inside: a difference on purpose);
//   - a mesh with vertices and no indices draws nothing;
//   - the material is a snapshot taken at DrawMesh (a change afterwards does not affect that draw);
//   - blending is stage G3b: a draw that asks for it is drawn opaque and says so once.
// The draws are exact: a quad on a pixel boundary covers exactly its pixels, so counts and colours are compared exactly.

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::RenderObject;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::WebGpu::WebGpuRenderer;
namespace ShaderInterface = Renderer::Gpu::ShaderInterface;

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

    int CountOccurrences(const std::string& text, const std::string& needle)
    {
        int count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        {
            ++count;
        }
        return count;
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

TEST_F(WebGpuDrawTest, ReadFramebufferIsRgbaWithTheBottomRowFirst)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* bottomHalf = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 0.0f, 0.0f));
    ASSERT_NE(red, nullptr);
    ASSERT_NE(bottomHalf, nullptr);

    const Frame frame = RenderFrameOf([&] { R().DrawMesh(bottomHalf, Identity, red); });

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

TEST_F(WebGpuDrawTest, ClipSpaceIsOpenGlsTheNearPlaneAtMinusOneTheFarPlaneAtOneAndNothingBeyond)
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
    Frame frame = RenderFrameOf([&] { R().DrawMesh(atNear, Identity, red); });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "z = -1 is on the near plane and inside the frustum";

    // Exactly on the far plane: drawn with the depth test off (the buffer is cleared to the far plane's 1.0 and LESS keeps
    // nothing that far, in GL too)
    frame = RenderFrameOf([&] { R().DrawMesh(atFar, Identity, green, noDepth); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "z = +1 is on the far plane and inside the frustum";

    // Just inside the far plane: drawn with the depth test on
    frame = RenderFrameOf([&] { R().DrawMesh(nearlyFar, Identity, green); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // Beyond either plane: clipped, whatever the depth state
    frame = RenderFrameOf([&]
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

TEST_F(WebGpuDrawTest, TheNearerQuadWinsWithTheDepthTestOnInEitherOrderAndTheLaterOneWinsWithItOff)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* nearQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, -0.5f)); // nearer in GL (smaller NDC z)
    IMesh* farQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    ASSERT_NE(farQuad, nullptr);

    // Depth test on (the default state): the nearer quad wins whichever is drawn first
    Frame frame = RenderFrameOf([&]
    {
        R().DrawMesh(farQuad, Identity, green);
        R().DrawMesh(nearQuad, Identity, red);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "far first, then near";
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(nearQuad, Identity, red);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "near first, then far: LESS keeps the near one";

    // Depth test off: the later draw wins, near or far
    RenderState noTest;
    noTest.depthTest = false;
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(nearQuad, Identity, red, noTest);
        R().DrawMesh(farQuad, Identity, green, noTest);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "later wins without a depth test";
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(farQuad, Identity, green, noTest);
        R().DrawMesh(nearQuad, Identity, red, noTest);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size);

    // Depth test on but depth write off: the near quad leaves no depth, so a later farther draw shows where it would
    // otherwise be hidden
    RenderState noWrite;
    noWrite.depthWrite = false;
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(nearQuad, Identity, red, noWrite);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "no depth was written by the near quad";
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(nearQuad, Identity, red); // writes depth
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "the contrast: with depth written the far quad stays hidden";

    // Nothing writes depth while the test is off (GL): a later draw that tests sees an untouched depth buffer
    RenderState testOffWriteOn;
    testOffWriteOn.depthTest = false;
    testOffWriteOn.depthWrite = true;
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(nearQuad, Identity, red, testOffWriteOn);
        R().DrawMesh(farQuad, Identity, green);
    });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "depthWrite means nothing with the depth test off";
}

TEST_F(WebGpuDrawTest, ACounterClockwiseTriangleShowsWithBackAndNoCullingAndAClockwiseOneTheOtherWayRound)
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
        const Frame ccwFrame = RenderFrameOf([&] { R().DrawMesh(ccw, Identity, red, state); });
        const Frame cwFrame = RenderFrameOf([&] { R().DrawMesh(cw, Identity, red, state); });
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

TEST_F(WebGpuDrawTest, TheDefaultStateCullsBackFacesAndDepthTestsAndWritesAsTheOpaqueQueueDoes)
{
    // RenderState{} is RenderState::Opaque(): the overload without a state draws with it
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* ccw = MakeMesh(Triangle(true));
    IMesh* cw = MakeMesh(Triangle(false));
    ASSERT_NE(cw, nullptr);
    const Frame front = RenderFrameOf([&] { R().DrawMesh(ccw, Identity, red); });
    const Frame back = RenderFrameOf([&] { R().DrawMesh(cw, Identity, red); });
    EXPECT_GT(front.Count(kRed), 1000);
    EXPECT_EQ(back.Count(kRed), 0);
}

TEST_F(WebGpuDrawTest, ABlendedDrawIsDrawnOpaqueInStageG3aAndSaysSoOnce)
{
    // Blending is stage G3b. Until then the state is in the pipeline key but every pipeline is opaque: a half-alpha red quad,
    // blended or additive, REPLACES what is behind it (as an opaque draw does), and the renderer logs it once per renderer
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    RenderState additive = RenderState::Transparent();
    additive.additive = true;

    ::testing::internal::CaptureStderr();
    const Frame blended = RenderFrameOf([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent()); });
    const Frame added = RenderFrameOf([&] { R().DrawMesh(quad, Identity, halfRed, additive); });
    const Frame opaque = RenderFrameOf([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Opaque()); });
    const std::string output = ::testing::internal::GetCapturedStderr();

    EXPECT_EQ(blended.Count(kRed), Size * Size);
    EXPECT_EQ(added.Count(kRed), Size * Size);
    EXPECT_EQ(opaque.Count(kRed), Size * Size);
    EXPECT_EQ(CountOccurrences(output, "blending is not implemented yet"), 1) << output;
}

// ============================================================================
// Index ranges (submeshes) and meshes without indices
// ============================================================================

TEST_F(WebGpuDrawTest, IndexRangeDrawsOnlyTheTrianglesInTheRange)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* mesh = MakeMesh(TwoHalves());
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->GetIndexCount(), 12u);

    // Only the right half
    Frame frame = RenderFrameOf([&] { R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{6, 6}); });
    Coverage c = CoverageOf(frame, kRed);
    EXPECT_EQ(c.count, Size * Size / 2);
    EXPECT_EQ(c.minX, Size / 2);
    EXPECT_EQ(CoverageOf(frame, kBlack).count, Size * Size / 2) << "the left half is untouched";

    // Each half with its own material: two submeshes, two colours
    frame = RenderFrameOf([&]
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
    frame = RenderFrameOf([&] { R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 12}); });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
    frame = RenderFrameOf([&] { R().DrawMesh(mesh, Identity, green); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // An empty range, or one starting at or past the end, draws nothing
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 0});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{12, 6});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{12, 0});
        R().DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{4000000000u, 6});
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);

    // A range that runs past the end draws NOTHING: the OpenGL renderer's rule, which Vulkan and WebGPU follow
    frame = RenderFrameOf([&] { R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{6, 600}); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "first + count is past the index count";
    frame = RenderFrameOf([&]
    {
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{0, 13});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{7, 6});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{4294967295u, 2});
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{1, 4294967295u});
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "including a sum that overflows 32 bits";

    // The last triangle of the buffer is still inside it
    frame = RenderFrameOf([&] { R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{9, 3}); });
    EXPECT_GT(frame.Count(kGreen), 0);
    EXPECT_LT(frame.Count(kGreen), Size * Size / 2);
}

TEST_F(WebGpuDrawTest, AMeshWithVerticesAndNoIndicesDrawsNothingAndBindsNoIndexBuffer)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    MeshData noIndices = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
    noIndices.indices.clear();
    IMesh* mesh = MakeMesh(noIndices);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetIndexCount(), 0u);
    EXPECT_EQ(mesh->GetVertexCount(), 4u);

    const Frame frame = RenderFrameOf([&]
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

TEST_F(WebGpuDrawTest, TheColourIsTheAlbedoTimesTheVertexColour)
{
    // Green vertices under a white material: green
    IMesh* greenVertices = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f));
    IMaterial* material = R().CreateMaterial(_unlit);
    ASSERT_NE(greenVertices, nullptr);
    ASSERT_NE(material, nullptr);
    Frame frame = RenderFrameOf([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size) << "the default albedo is white";

    // ...and times a yellow material: still green (yellow x green)
    material->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f);
    frame = RenderFrameOf([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // ...and times a red one: black
    material->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    frame = RenderFrameOf([&] { R().DrawMesh(greenVertices, Identity, material); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
}

TEST_F(WebGpuDrawTest, ANearestTextureIsSampledTimesTheAlbedoAndTheVertexColourWithExactColours)
{
    // 2 x 1 texels, red then blue, on a quad that spans the frame: the left half is the first texel
    const std::uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    ITexture* texture = R().CreateTexture(texels, 2, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);

    Frame frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); });
    EXPECT_EQ(frame.Count(kRed), Size * Size / 2);
    EXPECT_EQ(frame.Count(kBlue), Size * Size / 2);
    EXPECT_EQ(CoverageOf(frame, kRed).maxX, Size / 2 - 1) << "u = 0 is the left";
    EXPECT_EQ(CoverageOf(frame, kBlue).minX, Size / 2);

    // The texture is upright: a 1 x 2 texture, red in its first row (v = 0, the bottom) and blue in its second
    const std::uint8_t column[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    ITexture* upright = R().CreateTexture(column, 1, 2, 4, Nearest());
    IMaterial* uprightMaterial = R().CreateMaterial(_unlit, upright);
    ASSERT_NE(uprightMaterial, nullptr);
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, uprightMaterial); });
    EXPECT_EQ(frame.At(Size / 2, 8), kRed) << "v = 0 is the bottom of the quad (the first row of the texture)";
    EXPECT_EQ(frame.At(Size / 2, Size - 8), kBlue) << "v = 1 is the top";

    // Texture x albedo x vertex colour: a yellow texel, a cyan albedo and white vertices are green
    const std::uint8_t yellowTexel[4] = {255, 255, 0, 255};
    ITexture* yellow = R().CreateTexture(yellowTexel, 1, 1, 4, Nearest());
    IMaterial* cyanOnYellow = R().CreateMaterial(_unlit, yellow);
    ASSERT_NE(cyanOnYellow, nullptr);
    cyanOnYellow->SetColor("uAlbedo", 0.0f, 1.0f, 1.0f, 1.0f);
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, cyanOnYellow); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // ...and a vertex colour on top: yellow texel x white albedo x cyan vertices is green too
    IMesh* cyanVertices = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    IMaterial* plainYellow = R().CreateMaterial(_unlit, yellow);
    ASSERT_NE(plainYellow, nullptr);
    frame = RenderFrameOf([&] { R().DrawMesh(cyanVertices, Identity, plainYellow); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size);

    // uHasTexture 0 ignores the texture
    plainYellow->SetInt("uHasTexture", 0);
    frame = RenderFrameOf([&] { R().DrawMesh(cyanVertices, Identity, plainYellow); });
    EXPECT_EQ(frame.Count(Rgb{0, 255, 255}), Size * Size) << "white albedo x cyan vertices, no texture";
}

TEST_F(WebGpuDrawTest, TheTextureWrapRepeatsOrClampsAtTheEdgeAsTheOptionsSay)
{
    // Texture coordinates 0..2 over the quad (two repeats of a 2x1 texture, red then blue), one pixel per texel of the repeat
    MeshData twice = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
    for (Vertex& vertex : twice.vertices)
    {
        vertex.texCoord[0] *= 2.0f;
    }
    IMesh* quad = MakeMesh(twice);
    ASSERT_NE(quad, nullptr);
    const std::uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};

    TextureOptions repeat = Nearest();
    repeat.wrap = TextureWrap::Repeat;
    ITexture* repeating = R().CreateTexture(texels, 2, 1, 4, repeat);
    IMaterial* repeatingMaterial = R().CreateMaterial(_unlit, repeating);
    ASSERT_NE(repeatingMaterial, nullptr);
    Frame frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, repeatingMaterial); });
    // Quarters across: red, blue, red, blue
    EXPECT_EQ(frame.At(Size / 8, 10), kRed);
    EXPECT_EQ(frame.At(3 * Size / 8, 10), kBlue);
    EXPECT_EQ(frame.At(5 * Size / 8, 10), kRed) << "u 1..1.5 repeats the first texel";
    EXPECT_EQ(frame.At(7 * Size / 8, 10), kBlue);

    ITexture* clamping = R().CreateTexture(texels, 2, 1, 4, Nearest());
    IMaterial* clampingMaterial = R().CreateMaterial(_unlit, clamping);
    ASSERT_NE(clampingMaterial, nullptr);
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, clampingMaterial); });
    EXPECT_EQ(frame.At(Size / 8, 10), kRed);
    EXPECT_EQ(frame.At(3 * Size / 8, 10), kBlue);
    EXPECT_EQ(frame.At(5 * Size / 8, 10), kBlue) << "u past 1 clamps to the last texel";
    EXPECT_EQ(frame.At(7 * Size / 8, 10), kBlue);
}

TEST_F(WebGpuDrawTest, PixelsBelowTheAlphaCutoffAreSkippedColourAndDepth)
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
    Frame frame = RenderFrameOf([&] { R().DrawMesh(front, Identity, cutout); });
    EXPECT_EQ(frame.Count(kGreen), Size * Size / 2);
    EXPECT_EQ(frame.Count(kBlue), Size * Size / 2);

    // Cutoff 0.5: the transparent half is skipped, colour and depth, so the red quad behind it shows there
    cutout->SetFloat("uAlphaCutoff", 0.5f);
    frame = RenderFrameOf([&]
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
    frame = RenderFrameOf([&] { R().DrawMesh(back, Identity, red); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
}

// ============================================================================
// Material values are taken when the draw is recorded, as on OpenGL
// ============================================================================

TEST_F(WebGpuDrawTest, AChangeAfterDrawMeshDoesNotAffectThatDraw)
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

TEST_F(WebGpuDrawTest, TwoDrawsOfOneMaterialKeepTheColourEachWasDrawnWith)
{
    IMaterial* material = R().CreateMaterial(_unlit);
    IMesh* leftHalf = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f));
    IMesh* rightHalf = MakeMesh(Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(leftHalf, nullptr);
    ASSERT_NE(rightHalf, nullptr);

    const Frame frame = RenderFrameOf([&]
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

TEST_F(WebGpuDrawTest, ATextureChangedAfterDrawMeshDoesNotAffectThatDraw)
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

TEST_F(WebGpuDrawTest, AMaterialHasTheOpenGlDefaultsAndKeepsItsTextures)
{
    const std::uint8_t texel[4] = {255, 255, 255, 255};
    ITexture* texture = R().CreateTexture(texel, 1, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* plain = R().CreateMaterial(_unlit);
    IMaterial* textured = R().CreateMaterial(_unlit, texture);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(textured, nullptr);

    EXPECT_EQ(plain->GetShader(), _unlit);
    EXPECT_EQ(plain->GetTexture(), nullptr);
    EXPECT_EQ(textured->GetTexture(), texture);
    EXPECT_TRUE(plain->IsValid());

    const auto floatAt = [](const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto intAt = [](const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    std::vector<std::uint8_t> block;
    ASSERT_TRUE(R().GetMaterialBlockForTest(plain, block));
    ASSERT_EQ(block.size(), ShaderInterface::kMaterialBlockRange);
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        EXPECT_EQ(floatAt(block, channel * 4), 1.0f) << "uAlbedo is white";
    }
    EXPECT_EQ(intAt(block, 16), 0) << "uHasTexture is 0 without a texture";
    EXPECT_EQ(floatAt(block, 20), 0.0f) << "uAlphaCutoff is 0";
    ASSERT_TRUE(R().GetMaterialBlockForTest(textured, block));
    EXPECT_EQ(intAt(block, 16), 1) << "uHasTexture is 1 with a texture";

    // The values written by name land in the block as the table says (std140 offsets, WGSL-valid)
    plain->SetColor("uAlbedo", 0.25f, 0.5f, 0.75f, 1.0f);
    plain->SetFloat("uAlphaCutoff", 0.125f);
    plain->SetInt("uHasTexture", 1);
    ASSERT_TRUE(R().GetMaterialBlockForTest(plain, block));
    EXPECT_EQ(floatAt(block, 0), 0.25f);
    EXPECT_EQ(floatAt(block, 4), 0.5f);
    EXPECT_EQ(floatAt(block, 8), 0.75f);
    EXPECT_EQ(floatAt(block, 12), 1.0f);
    EXPECT_EQ(intAt(block, 16), 1);
    EXPECT_EQ(floatAt(block, 20), 0.125f);

    // Not this renderer's material
    FakeMaterial fake;
    EXPECT_FALSE(R().GetMaterialBlockForTest(&fake, block));
}

TEST_F(WebGpuDrawTest, AnUnknownUniformNameOrTheWrongTypeIsIgnoredAndTheMaterialStillDraws)
{
    IMaterial* material = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);

    ::testing::internal::CaptureStderr();
    material->SetFloat("uNoSuchThing", 3.0f);
    material->SetFloat("uNoSuchThing", 4.0f);
    material->SetInt("uAlbedo", 5); // the wrong type
    material->SetFloat("uMetallic", 1.0f); // a lit-shader uniform, not in the unlit block
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(CountOccurrences(output, "uNoSuchThing"), 1) << "logged once per name: " << output;

    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); }).Count(kRed), Size * Size);
}

// ============================================================================
// The matrices: row-major as the engine passes them, view and projection as last set
// ============================================================================

TEST_F(WebGpuDrawTest, TheModelMatrixIsRowMajorWithItsTranslationInTheLastColumn)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* centred = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(centred, nullptr);

    // Translated right by 0.5: the quad's x range -0.5..0.5 becomes 0..1, pixels 32..63 across and 16..47 up
    float model[16];
    Translation(model, 0.5f, 0.0f, 0.0f);
    Frame frame = RenderFrameOf([&] { R().DrawMesh(centred, model, red); });
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
    frame = RenderFrameOf([&] { R().DrawMesh(upperRight, quarterTurn, red); });
    EXPECT_EQ(frame.At(16, 48), kRed) << "the upper left";
    EXPECT_EQ(frame.At(48, 48), kBlack);
    EXPECT_EQ(frame.At(48, 16), kBlack) << "not the lower right";
    EXPECT_EQ(frame.At(16, 16), kBlack);
}

TEST_F(WebGpuDrawTest, TheViewAndTheProjectionAreRowMajorTooAndEachIsTransposedOnce)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(0.0f, -0.5f, 0.5f, 0.5f, 0.0f)); // x 0..0.5: pixels 32..47
    ASSERT_NE(quad, nullptr);

    // A translation of 0.25 in the view alone, in the projection alone, and in both (they add: 0.5): each moves the quad by
    // exactly that, which only a transpose applied exactly once per matrix does
    float quarter[16];
    Translation(quarter, 0.25f, 0.0f, 0.0f);
    struct Case
    {
        const float* view;
        const float* projection;
        int minX;
        const char* name;
    };
    const Case cases[] = {
        {quarter, Identity, 40, "view"},
        {Identity, quarter, 40, "projection"},
        {quarter, quarter, 48, "view and projection"},
        {Identity, Identity, 32, "neither"},
    };
    for (const Case& test : cases)
    {
        R().BeginFrame();
        R().SetViewProjection(test.view, test.projection);
        R().DrawMesh(quad, Identity, red);
        R().EndFrame();
        const Coverage c = CoverageOf(ReadFrame(), kRed);
        EXPECT_EQ(c.minX, test.minX) << test.name;
        EXPECT_EQ(c.count, 16 * 32) << test.name;
    }
}

TEST_F(WebGpuDrawTest, ASetViewProjectionInTheMiddleOfAFrameAffectsOnlyTheDrawsAfterIt)
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

TEST_F(WebGpuDrawTest, DrawObjectsIsTheSameDrawMeshCallsInOrderWithEachObjectsOwnState)
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

    const Frame viaObjects = RenderFrameOf([&] { R().DrawObjects(objects); });
    const Frame viaCalls = RenderFrameOf([&]
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

TEST_F(WebGpuDrawTest, DrawsOutsideAFrameAreIgnoredWithoutAMessage)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    std::vector<RenderObject> objects(1);
    objects[0].mesh = quad;
    objects[0].material = red;
    std::copy(std::begin(Identity), std::end(Identity), objects[0].transform.model);

    ::testing::internal::CaptureStderr();
    // Before the first frame, and between frames
    R().DrawMesh(quad, Identity, red);
    R().DrawMesh(quad, Identity, red, RenderState{}, IndexRange{0, 6});
    R().DrawObjects(objects);
    Frame frame = RenderFrameOf([&] {});
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
    R().DrawMesh(quad, Identity, red);
    R().DrawObjects(objects);
    frame = ReadFrame();
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "draws after EndFrame go nowhere";
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(output.find("WebGPU renderer:"), std::string::npos) << output;

    // Inside a frame, the same draws work
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
}

TEST_F(WebGpuDrawTest, ANullOrForeignMeshOrMaterialOrAnUnsetModelMatrixDrawsNothing)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    // Another renderer's mesh and material, and objects that are not this renderer's at all
    WebGpuRenderer& other = NewRenderer();
    ASSERT_TRUE(other.Initialize(nullptr, 8, 8)) << other.GetInitMessage();
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

    const Frame frame = RenderFrameOf([&]
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
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), Size * Size);
}

TEST_F(WebGpuDrawTest, ATextureDestroyedAfterTheMaterialTookItIsReplacedByTheDefaultTexture)
{
    const std::uint8_t redTexel[4] = {255, 0, 0, 255};
    ITexture* texture = R().CreateTexture(redTexel, 1, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", 1.0f, 1.0f, 0.0f, 1.0f); // yellow
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); }).Count(kRed), Size * Size)
        << "red texel x yellow albedo";
    R().DestroyTexture(texture);
    EXPECT_EQ(material->GetTexture(), texture) << "the material still names it";
    // The destroyed texture is white (the 1x1 default) for the draw: the albedo shows as it is, and nothing crashes
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); }).Count(kYellow), Size * Size);
}

TEST_F(WebGpuDrawTest, ATextureWhoseIdIsNotTheOneTheMaterialRecordedIsNotDrawnInItsPlace)
{
    // A texture made at the address of a destroyed one has a new id; a material that recorded the old id must not take it for
    // the texture it was given. The id is forged through the hook (the allocator does not promise to reuse an address)
    const std::uint8_t blueTexel[4] = {0, 0, 255, 255};
    ITexture* texture = R().CreateTexture(blueTexel, 1, 1, 4, Nearest());
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", 0.0f, 1.0f, 1.0f, 1.0f); // cyan
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);

    ASSERT_NE(R().GetTextureIdForTest(texture), 0u);
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); }).Count(kBlue), Size * Size)
        << "blue texel x cyan albedo";

    ASSERT_TRUE(R().SetMaterialTextureIdForTest(material, 0, R().GetTextureIdForTest(texture) + 1000));
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); }).Count(Rgb{0, 255, 255}), Size * Size)
        << "a stale id: the slot gets the white default, so the albedo shows as it is";
    EXPECT_FALSE(R().SetMaterialTextureIdForTest(material, 99, 1)) << "not a slot";
    FakeMaterial fake;
    EXPECT_FALSE(R().SetMaterialTextureIdForTest(&fake, 0, 1)) << "not this renderer's";
}

// ============================================================================
// Mid-frame destruction and updates
// ============================================================================

TEST_F(WebGpuDrawTest, DestroyingWhatADrawUsedBeforeTheFrameEndsKeepsTheDraw)
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
        // WebGPU keeps an object alive for every command that uses it: nothing is deferred and nothing is invalid
        R().DestroyMesh(quad);
        R().DestroyMaterial(material);
        R().DestroyTexture(texture);
        R().EndFrame();
        EXPECT_EQ(ReadFrame().Count(kGreen), Size * Size) << "frame " << frameNumber;
    }
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveTextureCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
    EXPECT_EQ(R().GetBindGroupCountForTest(), 0u) << "the bind groups that named the destroyed textures went with them";
}

TEST_F(WebGpuDrawTest, UpdateMeshAfterADrawLeavesThatDrawWithTheDataItWasDrawnWith)
{
    // The in-frame rule at the level of pixels: the first draw of a frame must keep the OLD vertices although UpdateMesh is
    // called before the frame's commands are submitted
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

TEST_F(WebGpuDrawTest, UpdateMeshWithAnotherSizeAfterADrawKeepsThatDrawToo)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* mesh = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 0.0f, 0.0f)); // the lower left quarter, 4 vertices
    ASSERT_NE(mesh, nullptr);
    const Frame frame = RenderFrameOf([&]
    {
        R().DrawMesh(mesh, Identity, red);
        ASSERT_TRUE(R().UpdateMesh(mesh, TwoHalves())); // 8 vertices, 12 indices: the whole frame
        R().DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{6, 6}); // the right half
    });
    EXPECT_EQ(mesh->GetVertexCount(), 8u);
    EXPECT_EQ(frame.Count(kRed), (Size / 2) * (Size / 2)) << "the first draw: the old quarter";
    EXPECT_EQ(frame.Count(kGreen), Size * Size / 2) << "the second: the right half of the new mesh";
}

// ============================================================================
// Wireframe
// ============================================================================

TEST_F(WebGpuDrawTest, WireframeIsIgnoredWithOneLogLineAndTheTriangleIsDrawnSolid)
{
    // Core WebGPU has no polygon mode (design decision W9): SetWireframe(true) is ignored, logged once per renderer
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* triangle = MakeMesh(Triangle(true));
    ASSERT_NE(triangle, nullptr);

    ::testing::internal::CaptureStderr();
    R().SetWireframe(true);
    R().SetWireframe(true);
    const Frame frame = RenderFrameOf([&] { R().DrawMesh(triangle, Identity, red); });
    R().SetWireframe(false);
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(CountOccurrences(output, "wireframe is not supported"), 1) << output;
    EXPECT_EQ(frame.At(Size / 2, Size / 2), kRed) << "the interior is filled";
}

// ============================================================================
// Pipelines, bind groups and the uniform ring
// ============================================================================

TEST_F(WebGpuDrawTest, TheUnlitShaderMakesAPipelineForEveryDepthWriteCullAndBlendCombinationAndCachesThem)
{
    // Distinct keys: the depth write only counts with the test on (3 depth states), 3 culls, and blend / additive (additive only
    // counts with blend: 3 blend states) = 27 pipelines, each made once
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    const CullMode culls[] = {CullMode::None, CullMode::Back, CullMode::Front};
    for (int round = 0; round < 2; ++round)
    {
        for (const bool test : {false, true})
        {
            for (const bool write : {false, true})
            {
                for (const CullMode cull : culls)
                {
                    for (const bool blend : {false, true})
                    {
                        for (const bool additive : {false, true})
                        {
                            RenderState state;
                            state.depthTest = test;
                            state.depthWrite = write;
                            state.cull = cull;
                            state.blend = blend;
                            state.additive = additive;
                            EXPECT_TRUE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Unlit, state));
                        }
                    }
                }
            }
        }
        EXPECT_EQ(R().GetPipelineCountForTest(), 27u) << "round " << round;
        EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 27u) << "a state seen again is a cache hit";
    }
    // The lit and text shaders are stage G3b
    EXPECT_FALSE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Lit, RenderState{}));
    EXPECT_FALSE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Text, RenderState{}));
}

TEST_F(WebGpuDrawTest, ThousandsOfDrawsMakeOnlyThePipelinesTheirStatesNeedAndOneBindGroupPerChunk)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    RenderState noCull;
    noCull.cull = CullMode::None;
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    for (int i = 0; i < 2000; ++i)
    {
        R().DrawMesh(quad, Identity, red, (i % 2 == 0) ? RenderState{} : noCull);
    }
    R().EndFrame();
    EXPECT_EQ(ReadFrame().Count(kRed), Size * Size);
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u);
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 2u);
    // One material bind group per ring chunk (every draw used the same, default, textures)
    EXPECT_EQ(R().GetBindGroupCountForTest(), R().GetRingChunkCountForTest());
}

TEST_F(WebGpuDrawTest, TenThousandDrawsInOneFrameAllLandTheRingGrowsAndIsReused)
{
    // 64 x 64 draws of one pixel each, every one with its own colour from one material changed between draws: a draw whose
    // blocks overlapped another's (a wrong alignment or offset) would show as a wrong pixel. A small chunk makes the ring chain
    // many buffers
    R().SetRingChunkBytesForTest(16 * 1024);
    IMaterial* material = R().CreateMaterial(_unlit);
    IMesh* pixel = MakeMesh(Quad(0.0f, 0.0f, 2.0f / Size, 2.0f / Size, 0.0f));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(pixel, nullptr);

    const auto colourOf = [](int x, int y) { return Rgb{static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4),
                                                        static_cast<std::uint8_t>(x + y)}; };
    const auto drawAll = [&]
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        for (int y = 0; y < Size; ++y)
        {
            for (int x = 0; x < Size; ++x)
            {
                const Rgb c = colourOf(x, y);
                material->SetColor("uAlbedo", static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                                   static_cast<float>(c.b) / 255.0f, 1.0f);
                float model[16];
                Translation(model, -1.0f + 2.0f * static_cast<float>(x) / Size, -1.0f + 2.0f * static_cast<float>(y) / Size, 0.0f);
                R().DrawMesh(pixel, model, material);
            }
        }
        R().EndFrame();
        return ReadFrame();
    };
    const auto allCorrect = [&](const Frame& frame)
    {
        int wrong = 0;
        for (int y = 0; y < Size; ++y)
        {
            for (int x = 0; x < Size; ++x)
            {
                wrong += frame.At(x, y) == colourOf(x, y) ? 0 : 1;
            }
        }
        return wrong;
    };

    const std::uint32_t alignment = R().GetUniformAlignmentForTest();
    ASSERT_GE(alignment, 4u);
    ASSERT_EQ(alignment & (alignment - 1), 0u) << "a power of two";

    EXPECT_EQ(allCorrect(drawAll()), 0) << "every one of the 4096 draws has its own colour";
    const std::uint32_t chunks = R().GetRingChunkCountForTest();
    EXPECT_GT(chunks, 1u) << "the ring chained more than one buffer";
    // The next frames reuse the chunks: no new buffers for the same load, and still every pixel right
    for (int frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        EXPECT_EQ(allCorrect(drawAll()), 0) << "frame " << frameNumber;
        EXPECT_EQ(R().GetRingChunkCountForTest(), chunks) << "the ring is reused";
    }
}

TEST_F(WebGpuDrawTest, PipelinesSurviveAResizeAndTheNextDrawIsStillExact)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), Size * Size);
    const std::uint32_t pipelines = R().GetPipelineCountForTest();
    ASSERT_GE(pipelines, 1u);

    ASSERT_TRUE(R().SetRenderTargetSize(32, 32));
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, red);
    R().EndFrame();
    std::vector<std::uint8_t> small(32 * 32 * 4);
    R().ReadFramebuffer(small.data(), 32, 32);
    EXPECT_TRUE(IsSolid(small, 32, 32, {255, 0, 0, 255}));
    EXPECT_EQ(R().GetPipelineCountForTest(), pipelines) << "the pipelines do not depend on the target's size";
}

TEST_F(WebGpuDrawTest, ADrawInTheMiddleOfAResizeIsDroppedAndTheNextFrameDrawsAgain)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(quad, Identity, red);
    ASSERT_TRUE(R().SetRenderTargetSize(Size, Size / 2)); // abandons the open frame
    R().DrawMesh(quad, Identity, red);                    // no frame is open: ignored
    R().EndFrame();                                       // nothing open any more
    ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), Size * Size);
}

TEST_F(WebGpuDrawTest, APipelineThatCannotBeMadeIsRememberedLoggedOncePerKeyAndItsDrawsAreSkipped)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    RenderState noCull;
    noCull.cull = CullMode::None;

    ::testing::internal::CaptureStderr();
    R().SetFailPipelineCreationForTest(true);
    Frame frame = RenderFrameOf([&]
    {
        for (int i = 0; i < 5; ++i)
        {
            R().DrawMesh(quad, Identity, red);
        }
    });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "every draw of the failing key is skipped";
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 1u) << "tried once, then remembered";
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);

    // The failure is remembered per key, also after the cause is gone; another key is tried on its own
    R().SetFailPipelineCreationForTest(false);
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); });
    EXPECT_EQ(frame.Count(kBlack), Size * Size) << "the remembered key stays skipped";
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 1u);
    frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, red, noCull); });
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "another state makes its own pipeline";
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 2u);
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(CountOccurrences(output, "could not create the render pipeline"), 1) << output;
}

TEST_F(WebGpuDrawTest, DestroyMaterialIgnoresNullForeignAndAlreadyDestroyedPointersAndShutdownFreesTheRest)
{
    IMaterial* a = R().CreateMaterial(_unlit);
    IMaterial* b = R().CreateMaterial(_unlit);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 2u);
    FakeMaterial fake;
    R().DestroyMaterial(nullptr);
    R().DestroyMaterial(&fake);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 2u);
    R().DestroyMaterial(a);
    R().DestroyMaterial(a); // twice
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 1u);
    // b is never destroyed: Shutdown frees it (the fixture checks that nothing is left referenced)
}

TEST_F(WebGpuDrawTest, ARendererInitializedAgainStartsCleanAndDrawsAgain)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), Size * Size);
    EXPECT_GE(R().GetPipelineCountForTest(), 1u);

    R().Shutdown();
    ASSERT_TRUE(R().Initialize(nullptr, Size, Size)) << R().GetInitMessage();
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMeshCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);

    // The old pointers are nothing to the new renderer
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); }).Count(kBlack), Size * Size);
    _unlit = R().GetStandardUnlitShader();
    ASSERT_NE(_unlit, nullptr);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* newQuad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(newQuad, nullptr);
    EXPECT_EQ(RenderFrameOf([&] { R().DrawMesh(newQuad, Identity, green); }).Count(kGreen), Size * Size);
}
