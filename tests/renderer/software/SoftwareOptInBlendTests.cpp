#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

// The software renderer's opt-in blending (issue #445, RenderState::softwareBlend): a blended draw that sets it is blended by its blend
// function (BlendFuncFor) as a GPU blends into an RGBA8 target; one that does not is still drawn opaque, so every other component and
// every existing software golden is unchanged. Sprite2D opts in (Sprite2DBlendTests.cpp draws it through a scene).
namespace
{
    constexpr int Size = 16;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<int, 4>;

    MeshData FullQuad()
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    ::testing::AssertionResult Near(const Pixel &actual, const Pixel &expected, const int tolerance = 1)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (std::abs(actual[i] - expected[i]) > tolerance)
            {
                return ::testing::AssertionFailure() << "(" << actual[0] << "," << actual[1] << "," << actual[2] << ") is not ("
                                                     << expected[0] << "," << expected[1] << "," << expected[2] << ")";
            }
        }
        return ::testing::AssertionSuccess();
    }

    class SoftwareOptInBlendTest : public ::testing::Test
    {
      protected:
        SoftwareRenderer renderer;
        IMesh *quad = nullptr;

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            quad = renderer.CreateMesh(FullQuad());
            ASSERT_NE(quad, nullptr);
        }
        void TearDown() override { renderer.Shutdown(); }

        IMaterial *Unlit(const float r, const float g, const float b, const float a)
        {
            IMaterial *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            material->SetColor("uAlbedo", r, g, b, a);
            return material;
        }

        /// A blended (Transparent queue) state with the given factors, opted in or not
        static RenderState Blended(const bool optIn, const bool additive = false, const BlendVariant variant = BlendVariant::Alpha)
        {
            RenderState state = RenderState::Transparent();
            state.cull = CullMode::None;
            state.additive = additive;
            state.variant = variant;
            state.softwareBlend = optIn;
            return state;
        }

        /// The middle pixel after `under` (opaque) and then `over` with `state`, on black
        Pixel Draw(IMaterial *under, IMaterial *over, const RenderState &state)
        {
            renderer.Clear(0, 0, 0, 1);
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            RenderState opaque = RenderState::Opaque();
            opaque.cull = CullMode::None;
            renderer.DrawMesh(quad, Identity, under, opaque);
            renderer.DrawMesh(quad, Identity, over, state);
            renderer.EndFrame();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Size) * Size * 4);
            renderer.ReadFramebuffer(pixels.data(), Size, Size);
            const std::size_t i = (static_cast<std::size_t>(Size / 2) * Size + Size / 2) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        }
    };
} // namespace

TEST_F(SoftwareOptInBlendTest, TheRendererSaysItBlendsOptedInDrawsOnly)
{
    EXPECT_FALSE(renderer.SupportsBlending()) << "other blended draws are still drawn opaque";
    EXPECT_TRUE(renderer.SupportsOptInBlending());
}

TEST_F(SoftwareOptInBlendTest, ABlendedDrawThatDoesNotOptInIsStillDrawnOpaque)
{
    IMaterial *blue = Unlit(0, 0, 1, 1);
    IMaterial *halfRed = Unlit(1, 0, 0, 0.5f);
    EXPECT_TRUE(Near(Draw(blue, halfRed, Blended(false)), {255, 0, 0, 0})) << "as before #445: no blending";
    EXPECT_TRUE(Near(Draw(blue, halfRed, Blended(false, true)), {255, 0, 0, 0})) << "additive is ignored too";
}

TEST_F(SoftwareOptInBlendTest, AlphaBlendingMixesByTheSourceAlpha)
{
    IMaterial *blue = Unlit(0, 0, 1, 1);
    EXPECT_TRUE(Near(Draw(blue, Unlit(1, 0, 0, 0.5f), Blended(true)), {128, 0, 127, 0}));
    EXPECT_TRUE(Near(Draw(blue, Unlit(1, 0, 0, 1), Blended(true)), {255, 0, 0, 0})) << "opaque covers";
    EXPECT_TRUE(Near(Draw(blue, Unlit(1, 0, 0, 0), Blended(true)), {0, 0, 255, 0})) << "transparent leaves what is behind";
}

TEST_F(SoftwareOptInBlendTest, AdditiveAddsTheSourceTimesItsAlpha)
{
    IMaterial *blue = Unlit(0, 0, 1, 1);
    EXPECT_TRUE(Near(Draw(blue, Unlit(1, 0, 0, 1), Blended(true, true)), {255, 0, 255, 0}));
    EXPECT_TRUE(Near(Draw(blue, Unlit(1, 0, 0, 0.5f), Blended(true, true)), {128, 0, 255, 0}));
    IMaterial *grey = Unlit(0.5f, 0.5f, 0.5f, 1);
    EXPECT_TRUE(Near(Draw(grey, Unlit(1, 1, 1, 1), Blended(true, true)), {255, 255, 255, 0})) << "clamped at white";
}

TEST_F(SoftwareOptInBlendTest, MultiplyIsTheTargetTimesTheSourceAndIgnoresAlpha)
{
    IMaterial *grey = Unlit(0.5f, 0.5f, 0.5f, 1);
    EXPECT_TRUE(Near(Draw(grey, Unlit(1, 0, 1, 0.25f), Blended(true, false, BlendVariant::Multiply)), {128, 0, 128, 0}));
}

TEST_F(SoftwareOptInBlendTest, PremultipliedAddsTheSourceAndKeepsWhatItsAlphaLeaves)
{
    IMaterial *blue = Unlit(0, 0, 1, 1);
    // ONE, ONE_MINUS_SRC_ALPHA: (0.5, 0, 0) + blue * 0.5
    EXPECT_TRUE(Near(Draw(blue, Unlit(0.5f, 0, 0, 0.5f), Blended(true, false, BlendVariant::Premultiplied)), {128, 0, 127, 0}));
}

TEST_F(SoftwareOptInBlendTest, TheStageDimAppliesBeforeTheBlendAsInTheGpuShaders)
{
    IMaterial *black = Unlit(0, 0, 0, 1);
    renderer.SetStageDim(1.0f);
    // Fully dimmed red is grey 0.299 * 0.45 of its luminance; added over black it is that grey
    const Pixel pixel = Draw(black, Unlit(1, 0, 0, 1), Blended(true, true));
    renderer.SetStageDim(0.0f);
    const int grey = static_cast<int>(0.299f * 0.45f * 255.0f + 0.5f);
    EXPECT_TRUE(Near(pixel, {grey, grey, grey, 0}, 2));
}
