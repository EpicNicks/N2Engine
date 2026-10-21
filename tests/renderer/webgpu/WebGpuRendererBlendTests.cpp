#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>
#include <renderer/common/GpuShaderInterface.hpp>

#include "common/BlendReference.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// Blending, depth write, the queues' states, a mid-frame SetViewProjection and dynamic meshes of the WebGPU backend (#43,
// stage D), at renderer level: real frames drawn with the built-in shaders on the offscreen target and read back, on a WebGPU
// device (Mesa lavapipe in CI), validation on (zero errors and warnings, nothing leaked at Shutdown).
//
// OpenGL is the behavioural reference (renderer/src/opengl/OpenGLRenderer.cpp ApplyRenderState): blend ->
// glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA) with the SAME factors for colour and alpha, so
//     out.rgb = a * src + (1 - a) * dst        (additive: a * src + dst)
//     out.a   = a * a + (1 - a) * dst.a        (additive: a * a + dst.a)
// clamped by the UNORM target; without blend an `additive` flag changes nothing (an opaque draw, alpha written as the
// shader gives it); glDepthMask(depthWrite) and no depth written while the depth test is off. The expectations are computed
// with BlendReference.hpp's formula (the same one the golden tests composite with) to within one unit, since the GPU blends
// in float and rounds once. The software renderer cannot blend, so none of these scenes is compared with a software frame.
//
// The 64x64 fixture draws with identity view and projection, so a vertex position is its NDC; a quad on a pixel boundary
// covers exactly its pixels.

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::SceneLightingData;

namespace
{
    /// Every pixel is (r, g, b) within `tolerance` per channel, and its alpha is `alpha` within `alphaTolerance` (when >= 0)
    ::testing::AssertionResult AllNear(const Frame& frame, const int r, const int g, const int b, const int tolerance,
                                       const int alpha = -1, const int alphaTolerance = 1)
    {
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb p = frame.At(x, y);
                const int a = frame.AlphaAt(x, y);
                const bool colourOff = std::abs(p.r - r) > tolerance || std::abs(p.g - g) > tolerance ||
                    std::abs(p.b - b) > tolerance;
                const bool alphaOff = alpha >= 0 && std::abs(a - alpha) > alphaTolerance;
                if (colourOff || alphaOff)
                {
                    return ::testing::AssertionFailure()
                        << "pixel (" << x << "," << y << ") is (" << int(p.r) << "," << int(p.g) << "," << int(p.b) << ","
                        << a << "), expected (" << r << "," << g << "," << b << "," << alpha << ") within " << tolerance
                        << " (alpha " << alphaTolerance << ")";
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    /// GL's blend of `source` (alpha `alpha`) over `destination`, per channel
    std::array<int, 3> Mix(const Rgb source, const double alpha, const Rgb destination, const bool additive = false)
    {
        return {Blend::BlendChannel(source.r, destination.r, alpha, additive),
                Blend::BlendChannel(source.g, destination.g, alpha, additive),
                Blend::BlendChannel(source.b, destination.b, alpha, additive)};
    }

    RenderState Additive()
    {
        RenderState state = RenderState::Transparent();
        state.additive = true;
        return state;
    }

    /// One quad per column: n quads, 4 pixels wide each (x = -1 + j / 8 .. + 1/8), over y -0.5..0.5 (pixels 16..47)
    MeshData Columns(const int n)
    {
        MeshData data;
        for (int j = 0; j < n; ++j)
        {
            const float x0 = -1.0f + static_cast<float>(j) / 8.0f;
            const MeshData quad = Quad(x0, -0.5f, x0 + 0.125f, 0.5f, 0.0f);
            const auto base = static_cast<std::uint32_t>(data.vertices.size());
            data.vertices.insert(data.vertices.end(), quad.vertices.begin(), quad.vertices.end());
            for (const std::uint32_t index : quad.indices)
            {
                data.indices.push_back(base + index);
            }
        }
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

    class WebGpuBlendTest : public WebGpuDrawTest
    {
    protected:
        /// A quad over the whole frame at depth z
        IMesh* Full(const float z) { return MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, z)); }

        /// Presents frames with nothing drawn: every frame slot is flushed (deferred deletions are freed)
        void FlushFrames(const int count = 3)
        {
            for (int i = 0; i < count; ++i)
            {
                R().BeginFrame();
                R().SetViewProjection(Identity, Identity);
                R().EndFrame();
            }
        }
    };
}

// ============================================================================
// Blending as OpenGL
// ============================================================================

TEST_F(WebGpuBlendTest, AHalfAlphaRedQuadOverAnOpaqueBlueOneIsGLsMixPerChannel)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, blue, RenderState::Opaque());
        R().DrawMesh(front, Identity, halfRed, RenderState::Transparent());
    });
    const std::array<int, 3> expected = Mix(kRed, 0.5, kBlue);
    EXPECT_TRUE(AllNear(frame, expected[0], expected[1], expected[2], 1));
    EXPECT_TRUE(AllNear(frame, 128, 0, 128, 1)) << "0.5 * 255 + 0.5 * 0 and 0.5 * 0 + 0.5 * 255";
}

TEST_F(WebGpuBlendTest, AHalfAlphaQuadOverTheClearColourBlendsWithTheClearColour)
{
    ClearTo(R(), Rgba{51, 102, 204, 255});
    IMesh* quad = Full(0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    const Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent()); });
    const std::array<int, 3> expected = Mix(kRed, 0.5, Rgb{51, 102, 204});
    EXPECT_TRUE(AllNear(frame, expected[0], expected[1], expected[2], 1));
    EXPECT_TRUE(AllNear(frame, 153, 51, 102, 1));
}

TEST_F(WebGpuBlendTest, AdditiveRedOverGreenIsYellowAndHalfAlphaAddsHalf)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);

    Frame frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, green, RenderState::Opaque());
        R().DrawMesh(front, Identity, red, Additive());
    });
    EXPECT_TRUE(AllNear(frame, 255, 255, 0, 0)) << "SRC_ALPHA (1) x red + green, exactly yellow";
    EXPECT_EQ(frame.Count(kYellow), Size * Size);

    frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, green, RenderState::Opaque());
        R().DrawMesh(front, Identity, halfRed, Additive());
    });
    const std::array<int, 3> expected = Mix(kRed, 0.5, kGreen, true);
    EXPECT_TRUE(AllNear(frame, expected[0], expected[1], expected[2], 1));
    EXPECT_TRUE(AllNear(frame, 128, 255, 0, 1)) << "0.5 x red added to green; green stays at 255";
}

TEST_F(WebGpuBlendTest, AdditiveSaturatesAt255)
{
    IMesh* quad = Full(0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, Additive()); });
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1)) << "one half-alpha additive draw over black";

    frame = RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, halfRed, Additive());
        R().DrawMesh(quad, Identity, halfRed, Additive());
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0)) << "128 + 128 clamps to 255, it does not wrap";

    frame = RenderFrame([&]
    {
        for (int i = 0; i < 5; ++i)
        {
            R().DrawMesh(quad, Identity, halfRed, Additive());
        }
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0)) << "and stays there";
}

TEST_F(WebGpuBlendTest, WithoutBlendTheAlphaIsIgnoredAndTheDrawIsOpaque)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, blue, RenderState::Opaque());
        R().DrawMesh(front, Identity, halfRed, RenderState::Opaque());
    });
    // Replaced, not mixed; as on GL the target's alpha is then the shader's (0.5), written as it is
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0, 128));
}

TEST_F(WebGpuBlendTest, AdditiveWithoutBlendIsAnOpaqueDrawAsOnGL)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    RenderState additiveOnly = RenderState::Opaque();
    additiveOnly.additive = true; // blend stays false
    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, green, RenderState::Opaque());
        R().DrawMesh(front, Identity, red, additiveOnly);
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0)) << "red replaces green: not yellow";
    EXPECT_EQ(frame.Count(kYellow), 0);
}

TEST_F(WebGpuBlendTest, TheReadBackAlphaIsGLsAlphaBlend)
{
    IMesh* quad = Full(0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);

    // Over a target of alpha 0: 0.5 * 0.5 + 0.5 * 0 = 0.25
    R().Clear(0.0f, 0.0f, 0.0f, 0.0f);
    Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent()); });
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, Blend::BlendAlpha(0.5, 0, false), 1));
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, 64, 1));

    // Over an opaque target: 0.25 + 0.5 * 1 = 0.75
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent()); });
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, Blend::BlendAlpha(0.5, 255, false), 1));
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, 191, 1));

    // Additive: 0.25 + dst alpha
    R().Clear(0.0f, 0.0f, 0.0f, 0.0f);
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, Additive()); });
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, 64, 1));
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    frame = RenderFrame([&] { R().DrawMesh(quad, Identity, halfRed, Additive()); });
    EXPECT_TRUE(AllNear(frame, 128, 0, 0, 1, 255, 1)) << "255 + 64 clamps";
}

TEST_F(WebGpuBlendTest, DrawOrderMattersAndEachOrderFollowsTheEquation)
{
    IMesh* quad = Full(0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    IMaterial* halfBlue = Colour(0.0f, 0.0f, 1.0f, 0.5f);

    const Frame redThenBlue = RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent());
        R().DrawMesh(quad, Identity, halfBlue, RenderState::Transparent());
    });
    const Frame blueThenRed = RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, halfBlue, RenderState::Transparent());
        R().DrawMesh(quad, Identity, halfRed, RenderState::Transparent());
    });

    // red over black = (128, 0, 0); blue over that = (64, 0, 128); and the other way round (128, 0, 64)
    const std::array<int, 3> first = Mix(kRed, 0.5, kBlack);
    const std::array<int, 3> redBlue = Mix(kBlue, 0.5, Rgb{static_cast<std::uint8_t>(first[0]), 0, 0});
    EXPECT_TRUE(AllNear(redThenBlue, redBlue[0], redBlue[1], redBlue[2], 1));
    EXPECT_TRUE(AllNear(redThenBlue, 64, 0, 128, 1));
    EXPECT_TRUE(AllNear(blueThenRed, 128, 0, 64, 1));
    EXPECT_NE(redThenBlue, blueThenRed);
}

TEST_F(WebGpuBlendTest, AlphaZeroLeavesTheTargetUntouched)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMaterial* invisible = Colour(1.0f, 0.0f, 0.0f, 0.0f);
    Frame frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, blue, RenderState::Opaque());
        R().DrawMesh(front, Identity, invisible, RenderState::Transparent());
    });
    EXPECT_TRUE(AllNear(frame, 0, 0, 255, 0, 255, 0));
    frame = RenderFrame([&]
    {
        R().DrawMesh(back, Identity, blue, RenderState::Opaque());
        R().DrawMesh(front, Identity, invisible, Additive());
    });
    EXPECT_TRUE(AllNear(frame, 0, 0, 255, 0, 255, 0)) << "additive: SRC_ALPHA (0) x red adds nothing";
}

TEST_F(WebGpuBlendTest, BlendedDrawsStayInSubmissionOrderWithAnOpaqueDrawBetweenThem)
{
    // The renderer never reorders: opaque, blended, opaque, blended
    IMesh* quad = Full(0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMaterial* halfRed = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    IMaterial* halfBlue = Colour(0.0f, 0.0f, 1.0f, 0.5f);
    RenderState opaqueAlways = RenderState::Opaque();
    opaqueAlways.depthTest = false; // the same depth: only the order decides
    RenderState blendedAlways = RenderState::Transparent();
    blendedAlways.depthTest = false;
    const Frame frame = RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, halfRed, blendedAlways);  // over black: (128, 0, 0)
        R().DrawMesh(quad, Identity, green, opaqueAlways);     // replaces it: (0, 255, 0)
        R().DrawMesh(quad, Identity, halfBlue, blendedAlways); // over green: (0, 128, 128)
    });
    EXPECT_TRUE(AllNear(frame, 0, 128, 128, 1));
}

// ============================================================================
// Depth write
// ============================================================================

TEST_F(WebGpuBlendTest, ATransparentDrawIsDepthTestedAgainstOpaqueGeometryButWritesNoDepth)
{
    IMesh* nearQuad = Full(-0.5f);
    IMesh* farQuad = Full(0.5f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* halfGreen = Colour(0.0f, 1.0f, 0.0f, 0.5f);

    // Behind an opaque draw: depth tested, so hidden entirely
    Frame frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, RenderState::Opaque());
        R().DrawMesh(farQuad, Identity, halfGreen, RenderState::Transparent());
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0)) << "a Transparent draw behind opaque geometry is hidden";

    // In front, and an opaque quad drawn LATER behind it: the Transparent draw wrote no depth, so the opaque one
    // overwrites it (the mirror of SoftwareTextShaderTest.OtherTransparentDrawsWriteNoDepth, with GL's expectation: the
    // software renderer has the same outcome here by reordering, WebGPU by not writing depth)
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, halfGreen, RenderState::Transparent());
        R().DrawMesh(farQuad, Identity, red, RenderState::Opaque());
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0)) << "no depth written by the Transparent draw: the later quad behind it wins";

    // The contrast: the opaque quad first, then the Transparent one in front: a real blend over it
    frame = RenderFrame([&]
    {
        R().DrawMesh(farQuad, Identity, red, RenderState::Opaque());
        R().DrawMesh(nearQuad, Identity, halfGreen, RenderState::Transparent());
    });
    const std::array<int, 3> expected = Mix(kGreen, 0.5, kRed);
    EXPECT_TRUE(AllNear(frame, expected[0], expected[1], expected[2], 1));
    EXPECT_TRUE(AllNear(frame, 128, 128, 0, 1));
}

TEST_F(WebGpuBlendTest, ADrawWithTheDepthTestOffWritesNoDepthWhateverDepthWriteSays)
{
    IMesh* nearQuad = Full(-0.5f);
    IMesh* farQuad = Full(0.5f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);

    RenderState testOffWriteOn;
    testOffWriteOn.depthTest = false;
    testOffWriteOn.depthWrite = true; // GL: nothing writes depth while the test is off
    Frame frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, testOffWriteOn);
        R().DrawMesh(farQuad, Identity, green, RenderState::Opaque());
    });
    EXPECT_TRUE(AllNear(frame, 0, 255, 0, 0)) << "the farQuad quad passes: the first wrote no depth";

    // The contrast: the same draw with the test on does write depth, and hides the farQuad quad
    RenderState testOnWriteOn;
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, testOnWriteOn);
        R().DrawMesh(farQuad, Identity, green, RenderState::Opaque());
    });
    EXPECT_TRUE(AllNear(frame, 255, 0, 0, 0));

    // And depth write off with the test on (the Transparent state without blending)
    RenderState testOnWriteOff;
    testOnWriteOff.depthWrite = false;
    frame = RenderFrame([&]
    {
        R().DrawMesh(nearQuad, Identity, red, testOnWriteOff);
        R().DrawMesh(farQuad, Identity, green, RenderState::Opaque());
    });
    EXPECT_TRUE(AllNear(frame, 0, 255, 0, 0));
}

// ============================================================================
// Pipelines
// ============================================================================

TEST_F(WebGpuBlendTest, BlendPipelinesAreMadeOncePerKeyAndAdditiveWithoutBlendSharesTheOpaqueOne)
{
    IMesh* quad = Full(0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    ASSERT_EQ(R().GetPipelineCountForTest(), 0u);

    RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 1u);

    RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Transparent()); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u) << "a blended key is a new pipeline";
    RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, red, RenderState::Transparent());
        R().DrawMesh(quad, Identity, red, RenderState::Transparent());
    });
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u) << "a repeated key reuses it";

    RenderFrame([&] { R().DrawMesh(quad, Identity, red, Additive()); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u) << "additive blending is another";

    RenderState additiveOnly = RenderState::Opaque();
    additiveOnly.additive = true;
    RenderFrame([&] { R().DrawMesh(quad, Identity, red, additiveOnly); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u) << "additive without blend is the opaque draw: no new pipeline";
    EXPECT_TRUE(R().CreatePipelineForTest(Renderer::Gpu::ShaderInterface::BuiltInShader::Unlit, additiveOnly));
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u);

    // Depth write still distinguishes pipelines (the same blend, writing depth or not)
    RenderState blendedWriting = RenderState::Transparent();
    blendedWriting.depthWrite = true;
    RenderFrame([&] { R().DrawMesh(quad, Identity, red, blendedWriting); });
    EXPECT_EQ(R().GetPipelineCountForTest(), 4u);
}

TEST_F(WebGpuBlendTest, BlendedDrawsLogNothingAboutBlending)
{
    IMesh* quad = Full(0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f, 0.5f);
    CerrCapture capture;
    RenderFrame([&]
    {
        R().DrawMesh(quad, Identity, red, RenderState::Transparent());
        R().DrawMesh(quad, Identity, red, Additive());
    });
    const std::string text = capture.Text();
    EXPECT_EQ(text.find("blend"), std::string::npos) << text;
    EXPECT_EQ(text.find("stage D"), std::string::npos) << text;
}

// ============================================================================
// Blended LIT draws: Gamma follows the equation; Linear is a documented divergence
// ============================================================================

TEST_F(WebGpuBlendTest, ABlendedLitDrawInGammaLightingFollowsTheEquation)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMaterial* lit = R().CreateMaterial(R().GetStandardLitShader());
    ASSERT_NE(lit, nullptr);
    lit->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 0.5f);
    SceneLightingData lighting; // Gamma by default
    lighting.ambientColor = N2Engine::Math::Vector3(1.0f, 1.0f, 1.0f);
    ASSERT_EQ(lighting.colorSpace, Renderer::Common::ColorSpace::Gamma);

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(lighting, N2Engine::Math::Vector3(0.0f, 0.0f, 100.0f));
    R().DrawMesh(back, Identity, blue, RenderState::Opaque());
    R().DrawMesh(front, Identity, lit, RenderState::Transparent());
    R().EndFrame();
    const Frame frame = ReadFrame();
    const std::array<int, 3> expected = Mix(kRed, 0.5, kBlue);
    EXPECT_TRUE(AllNear(frame, expected[0], expected[1], expected[2], 1));
    EXPECT_TRUE(AllNear(frame, 128, 0, 128, 1));
}

TEST_F(WebGpuBlendTest, ABlendedLitDrawInLinearLightingBlendsTheEncodedValuesUnlikeGL)
{
    // KNOWN DIVERGENCE (stage D, listed in the PR): on OpenGL a blended LIT draw in LINEAR lighting blends in linear space
    // (the framebuffer is SRGB8_ALPHA8 and GL_FRAMEBUFFER_SRGB is on for that draw), so half-alpha red over blue would give
    // about (188, 0, 188). WebGPU's target is R8G8B8A8_UNORM and the lit shader encodes in the shader, so it blends the
    // ENCODED values like every other draw: (128, 0, 128). Unlit, text, UI, particle and Gamma-lit blends match GL. This test
    // documents the WebGPU result; it does not assert equality with GL.
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* blue = Colour(0.0f, 0.0f, 1.0f);
    IMaterial* lit = R().CreateMaterial(R().GetStandardLitShader());
    ASSERT_NE(lit, nullptr);
    lit->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 0.5f);
    SceneLightingData lighting;
    lighting.ambientColor = N2Engine::Math::Vector3(1.0f, 1.0f, 1.0f);
    lighting.colorSpace = Renderer::Common::ColorSpace::Linear;

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(lighting, N2Engine::Math::Vector3(0.0f, 0.0f, 100.0f));
    R().DrawMesh(back, Identity, blue, RenderState::Opaque());
    R().DrawMesh(front, Identity, lit, RenderState::Transparent());
    R().EndFrame();
    const Frame frame = ReadFrame();
    // The lit colour is red 1 -> encoded 1 (255): the encoded-space equation, as in Gamma lighting
    const std::array<int, 3> encodedSpace = Mix(kRed, 0.5, kBlue);
    EXPECT_TRUE(AllNear(frame, encodedSpace[0], encodedSpace[1], encodedSpace[2], 1));
    const int glLinearSpace = 188; // sRGB encode of 0.5 in linear light, what GL would show in the red and blue channels
    EXPECT_GT(std::abs(static_cast<int>(frame.At(32, 32).r) - glLinearSpace), 40) << "not GL's linear-space blend";
}

// ============================================================================
// A mid-frame SetViewProjection (the UI pass after the scene)
// ============================================================================

TEST_F(WebGpuBlendTest, AScenePassAndAUiPassInOneFrameNeitherMovesTheOther)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* halfGreen = Colour(0.0f, 1.0f, 0.0f, 0.5f);
    IMesh* sceneQuad = MakeMesh(Quad(-0.25f, -0.25f, 0.25f, 0.25f, 0.0f)); // the scene's: pixels 24..39 before the camera moves

    // The scene pass: a view that moves the quad left by 0.5 (16 pixels): x 8..23, y 24..39
    float sceneView[16];
    Translation(sceneView, -0.5f, 0.0f, 0.0f);

    // The UI pass: identity view and an orthographic projection over 64 x 64 pixel units (y up), the overlay state of
    // UISystem::OverlayState (no depth test or write, no cull, blended). Its quad is in pixel units: x 16..47, y 20..43
    const float overlayProjection[16] = {2.0f / 64.0f, 0, 0, -1.0f, 0, 2.0f / 64.0f, 0, -1.0f, 0, 0, 1, 0, 0, 0, 0, 1};
    IMesh* uiQuad = MakeMesh(Quad(16.0f, 20.0f, 48.0f, 44.0f, 0.0f));
    RenderState overlay;
    overlay.depthTest = false;
    overlay.depthWrite = false;
    overlay.cull = CullMode::None;
    overlay.blend = true;

    R().BeginFrame();
    R().SetViewProjection(sceneView, Identity);
    R().DrawMesh(sceneQuad, Identity, red, RenderState::Opaque());
    R().SetViewProjection(Identity, overlayProjection);
    R().DrawMesh(uiQuad, Identity, halfGreen, overlay);
    R().EndFrame();
    const Frame frame = ReadFrame();

    // The scene did not move: its red is where the scene's camera put it (x 8..23) minus what the overlay covers (x >= 16)
    const Coverage red8to15 = CoverageOf(frame, kRed);
    EXPECT_EQ(red8to15.count, 8 * 16) << "x 8..15 of the scene quad's 16 columns, 16 rows";
    EXPECT_EQ(red8to15.minX, 8);
    EXPECT_EQ(red8to15.maxX, 15);
    EXPECT_EQ(red8to15.minY, 24);
    EXPECT_EQ(red8to15.maxY, 39);

    // The overlay was not moved by the scene's view: over black where the scene is not, over red where it is
    const std::array<int, 3> overBlack = Mix(kGreen, 0.5, kBlack);
    const std::array<int, 3> overRed = Mix(kGreen, 0.5, kRed);
    const Rgb overlayOnly = frame.At(40, 30);
    EXPECT_NEAR(overlayOnly.r, overBlack[0], 1);
    EXPECT_NEAR(overlayOnly.g, overBlack[1], 1);
    EXPECT_NEAR(overlayOnly.b, overBlack[2], 1);
    const Rgb overlap = frame.At(20, 30);
    EXPECT_NEAR(overlap.r, overRed[0], 1);
    EXPECT_NEAR(overlap.g, overRed[1], 1);
    EXPECT_NEAR(overlap.b, overRed[2], 1);
    EXPECT_EQ(frame.At(60, 60), kBlack);
    EXPECT_EQ(frame.At(10, 50), kBlack) << "above the overlay and the scene quad";
    // The overlay's own extent: x 16..47, y 20..43 (a 32 x 24 rect), every pixel of it touched
    int touched = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const bool inside = x >= 16 && x <= 47 && y >= 20 && y <= 43;
            const Rgb p = frame.At(x, y);
            if (inside)
            {
                EXPECT_GT(p.g, 100) << x << "," << y;
                ++touched;
            }
        }
    }
    EXPECT_EQ(touched, 32 * 24);
}

TEST_F(WebGpuBlendTest, ManyViewProjectionChangesInOneFramePastOneRingChunkEachDrawKeepsItsOwn)
{
    // One draw per pixel: draw i covers ONLY pixel (i % 64, i / 64), and only because of its own view (a translation that
    // moves a one-pixel quad from pixel (0, 0) to its cell), set just before it. No two draws share a pixel, so nothing is
    // hidden by depth or by a later draw: if any draw took the wrong matrices (a stale ring block, the previous chunk's, the
    // next draw's, the first draw's), its own pixel stays the clear colour and another pixel shows the wrong colour. The
    // 4096 draws per frame span several ring chunks (192 bytes of matrices plus the material block per draw, the first chunk
    // 256 KiB), so draws in the later chunks are checked pixel by pixel like those in the first. The colour of draw i is
    // (i + i / 64) % 4, so every pixel differs from its four neighbours (a draw moved by one cell shows the wrong colour too).
    constexpr int kDraws = Size * Size;
    IMaterial* materials[4] = {Colour(1.0f, 0.0f, 0.0f), Colour(0.0f, 1.0f, 0.0f), Colour(0.0f, 0.0f, 1.0f),
                               Colour(1.0f, 1.0f, 0.0f)};
    const Rgb colours[4] = {kRed, kGreen, kBlue, kYellow};
    const float cell = 2.0f / static_cast<float>(Size); // one pixel in NDC (identity projection)
    IMesh* pixel = MakeMesh(Quad(-1.0f, -1.0f, -1.0f + cell, -1.0f + cell, 0.0f)); // pixel (0, 0) exactly
    ASSERT_NE(pixel, nullptr);
    // No depth: the order of submission alone decides, and no pixel is drawn twice anyway
    RenderState noDepth;
    noDepth.depthTest = false;
    noDepth.depthWrite = false;
    const std::uint32_t chunksBefore = R().GetRingChunkCountForTest();

    for (int frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        for (int i = 0; i < kDraws; ++i)
        {
            // The renderer copies the matrices at SetViewProjection, so a view on the stack is enough
            float view[16];
            Translation(view, cell * static_cast<float>(i % Size), cell * static_cast<float>(i / Size), 0.0f);
            R().SetViewProjection(view, Identity);
            R().DrawMesh(pixel, Identity, materials[(i + i / Size) % 4], noDepth);
        }
        R().EndFrame();
        const Frame frame = ReadFrame();
        int wrong = 0;
        for (int i = 0; i < kDraws; ++i)
        {
            const int x = i % Size;
            const int y = i / Size;
            const Rgb expected = colours[(i + i / Size) % 4];
            if (frame.At(x, y) != expected)
            {
                // Report the first few only; the count says how many draws went astray
                if (++wrong <= 8)
                {
                    ADD_FAILURE() << "frame " << frameNumber << ", draw " << i << ": pixel (" << x << "," << y << ") is ("
                                  << int(frame.At(x, y).r) << "," << int(frame.At(x, y).g) << ","
                                  << int(frame.At(x, y).b) << "), expected (" << int(expected.r) << ","
                                  << int(expected.g) << "," << int(expected.b) << ")";
                }
            }
        }
        EXPECT_EQ(wrong, 0) << "frame " << frameNumber << ": draws whose pixel is not their own colour";
    }
    EXPECT_GE(R().GetRingChunkCountForTest(), chunksBefore + 2)
        << "the 4096 draws of a frame spanned at least three ring chunks";
}

// ============================================================================
// Dynamic meshes: UpdateMesh every frame
// ============================================================================

TEST_F(WebGpuBlendTest, AMeshUpdatedEveryFrameDrawsItsOwnGeometryEachFrameAndLeaksNothing)
{
    constexpr int kFramesInFlight = 2; // the Vulkan backend's frames in flight: WebGPU has no such limit, the sequence is kept
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* mesh = MakeMesh(Columns(1));
    ASSERT_NE(mesh, nullptr);
    RenderFrame([&] { R().DrawMesh(mesh, Identity, red, RenderState::Transparent()); });
    FlushFrames();
    const std::uint32_t meshes = R().GetLiveMeshCountForTest();
    const std::uint32_t pipelines = R().GetPipelineCountForTest();

    // More than twice the frames in flight; the vertex count grows, shrinks, and goes to none and back
    const std::vector<int> counts = {1, 3, 16, 2, 0, 5, 1, 0, 0, 8, 4, 12, 1, 7};
    ASSERT_GT(counts.size(), static_cast<std::size_t>(2 * kFramesInFlight));
    for (std::size_t frameNumber = 0; frameNumber < counts.size(); ++frameNumber)
    {
        const int n = counts[frameNumber];
        if (n == 0)
        {
            // The renderer refuses an update with nothing in it and keeps the mesh as it was; a frame that draws nothing
            // (as ParticleSystem does with no particles alive) leaves the target as cleared
            EXPECT_FALSE(R().UpdateMesh(mesh, MeshData{}));
            const Frame frame = RenderFrame([] {});
            EXPECT_EQ(frame.Count(kBlack), Size * Size) << "frame " << frameNumber;
            continue;
        }
        ASSERT_TRUE(R().UpdateMesh(mesh, Columns(n))) << "frame " << frameNumber;
        EXPECT_EQ(mesh->GetVertexCount(), static_cast<std::uint32_t>(4 * n));
        EXPECT_EQ(mesh->GetIndexCount(), static_cast<std::uint32_t>(6 * n));
        const Frame frame = RenderFrame([&] { R().DrawMesh(mesh, Identity, red, RenderState::Transparent()); });
        EXPECT_EQ(frame.Count(kRed), n * 4 * 32) << "frame " << frameNumber << ": its own " << n << " quads";
        for (int j = 0; j < 16; ++j)
        {
            EXPECT_EQ(frame.At(4 * j + 1, 32), j < n ? kRed : kBlack) << "frame " << frameNumber << ", column " << j;
        }
    }
    EXPECT_EQ(R().GetPipelineCountForTest(), pipelines) << "updating a mesh makes no pipeline";

    FlushFrames();
    EXPECT_EQ(R().GetLiveMeshCountForTest(), meshes);
    // WebGPU frees a replaced buffer itself once the commands that used it are done: the renderer holds nothing back, and the
    // fixture's leak check at teardown (every WebGPU reference released) is the proof that no buffer of an earlier update is left
}

TEST_F(WebGpuBlendTest, UpdatingAndThenDestroyingAMeshInTheFrameThatDrewItStillDrawsTheFrame)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    IMesh* mesh = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 0.0f, 0.0f)); // the lower left quarter
    ASSERT_NE(mesh, nullptr);
    RenderFrame([&] { R().DrawMesh(mesh, Identity, red, RenderState::Transparent()); });
    FlushFrames();
    const std::uint32_t meshes = R().GetLiveMeshCountForTest();

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(mesh, Identity, red, RenderState::Transparent()); // recorded with the first buffers
    ASSERT_TRUE(R().UpdateMesh(mesh, Quad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f))); // the upper right quarter, new buffers
    R().DrawMesh(mesh, Identity, green, RenderState::Transparent());
    R().DestroyMesh(mesh); // in the frame that drew it, twice over
    R().EndFrame();
    const Frame frame = ReadFrame();
    EXPECT_EQ(frame.Count(kRed), Size * Size / 4) << "the draw recorded before the update used the old buffers";
    EXPECT_EQ(frame.Count(kGreen), Size * Size / 4) << "the one after it the new";
    EXPECT_EQ(frame.At(16, 16), kRed);
    EXPECT_EQ(frame.At(48, 48), kGreen);

    FlushFrames();
    EXPECT_EQ(R().GetLiveMeshCountForTest(), meshes - 1u);
    // The mesh's buffers, the first pair and the pair the update made, are released with it (the fixture's leak check)
}
