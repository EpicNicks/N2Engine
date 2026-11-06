#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <vector>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

// RenderState::dim and IRenderer::SetStageDim (the editor's prefab stage, In Scene mode: the scene behind the prefab is
// drawn dimmed). The software renderer is the reference the GPU backends' shaders implement (RenderState::StageDimmed).
namespace
{
    constexpr int Size = 64;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<std::uint8_t, 4>;
    constexpr Pixel Black{0, 0, 0, 255}, Red{255, 0, 0, 255}, Green{0, 255, 0, 255};

    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        Pixel At(int x, int y) const
        {
            const auto i = (y * Size + x) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        }
    };

    /// A quad over the whole frame at `depth` (clip space z), facing the camera
    MeshData Quad(const float depth)
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, depth}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{1, -1, depth}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{1, 1, depth}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{-1, 1, depth}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// What RenderState::StageDimmed makes of an 8-bit colour, packed as the renderer packs it
    Pixel Dimmed(const Pixel &pixel, const float dim)
    {
        float r = pixel[0] / 255.f, g = pixel[1] / 255.f, b = pixel[2] / 255.f;
        RenderState::StageDimmed(r, g, b, dim);
        const auto to8 = [](float x) { return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
        return {to8(r), to8(g), to8(b), pixel[3]};
    }

    class SoftwareStageDimTest : public ::testing::Test
    {
      protected:
        SoftwareRenderer renderer;
        IMaterial *red = nullptr, *green = nullptr;
        IMesh *near = nullptr, *far = nullptr;

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            renderer.Clear(0, 0, 0, 1);
            red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            green = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            near = renderer.CreateMesh(Quad(-0.5f));
            far = renderer.CreateMesh(Quad(0.5f));
            ASSERT_NE(red, nullptr);
            ASSERT_NE(green, nullptr);
            ASSERT_NE(near, nullptr);
            ASSERT_NE(far, nullptr);
            red->SetColor("uAlbedo", 1, 0, 0, 1);
            green->SetColor("uAlbedo", 0, 1, 0, 1);
        }
        void TearDown() override
        {
            renderer.SetStageDim(0.0f);
            renderer.Shutdown();
        }

        template <class Draw> Frame Render(Draw draw)
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draw();
            renderer.EndFrame();
            Frame frame{std::vector<std::uint8_t>(Size * Size * 4)};
            renderer.ReadFramebuffer(frame.pixels.data(), Size, Size);
            return frame;
        }
    };
} // namespace

TEST(StageDimMaths, ZeroChangesNothingAndOneIsTheDarkenedGreyOfTheLuminance)
{
    float r = 1.f, g = 0.5f, b = 0.25f;
    RenderState::StageDimmed(r, g, b, 0.f);
    EXPECT_FLOAT_EQ(r, 1.f);
    EXPECT_FLOAT_EQ(g, 0.5f);
    EXPECT_FLOAT_EQ(b, 0.25f);

    RenderState::StageDimmed(r, g, b, 1.f);
    const float grey = (RenderState::DimLumaR * 1.f + RenderState::DimLumaG * 0.5f + RenderState::DimLumaB * 0.25f) *
                       RenderState::DimGrey;
    EXPECT_FLOAT_EQ(r, grey);
    EXPECT_FLOAT_EQ(g, grey);
    EXPECT_FLOAT_EQ(b, grey);

    float r2 = 0.f, g2 = 1.f, b2 = 0.f;
    RenderState::StageDimmed(r2, g2, b2, 0.5f);
    EXPECT_LT(g2, 1.f) << "darker";
    EXPECT_GT(r2, 0.f) << "and less saturated";
}

TEST(StageDimMaths, TheRenderersModifierRaisesADrawsDimAndIsClamped)
{
    SoftwareRenderer renderer;
    EXPECT_TRUE(renderer.SupportsStageDim());
    EXPECT_FLOAT_EQ(renderer.WithStageDim(RenderState{}).dim, 0.f);
    renderer.SetStageDim(2.f);
    EXPECT_FLOAT_EQ(renderer.GetStageDim(), 1.f);
    renderer.SetStageDim(0.4f);
    EXPECT_FLOAT_EQ(renderer.WithStageDim(RenderState{}).dim, 0.4f);
    RenderState dimmer;
    dimmer.dim = 0.7f;
    EXPECT_FLOAT_EQ(renderer.WithStageDim(dimmer).dim, 0.7f) << "the larger of the two";
    renderer.SetStageDim(-1.f);
    EXPECT_FLOAT_EQ(renderer.GetStageDim(), 0.f);
}

TEST_F(SoftwareStageDimTest, ADrawRecordedWhileTheStageDimIsSetIsDimmedAndTheNextIsNot)
{
    const Frame plain = Render([&] { renderer.DrawMesh(far, Identity, red); });
    EXPECT_EQ(plain.At(32, 32), Red);

    const Frame dimmed = Render([&] {
        renderer.SetStageDim(0.65f);
        renderer.DrawMesh(far, Identity, red);
        renderer.SetStageDim(0.0f);
    });
    EXPECT_EQ(dimmed.At(32, 32), Dimmed(Red, 0.65f));
    EXPECT_NE(dimmed.At(32, 32), Red);

    // The state's own dim, without the modifier
    RenderState state;
    state.dim = 1.0f;
    const Frame own = Render([&] { renderer.DrawMesh(far, Identity, red, state); });
    EXPECT_EQ(own.At(32, 32), Dimmed(Red, 1.0f));
}

TEST_F(SoftwareStageDimTest, TheDimStaysWithItsDrawWhenTheRendererReordersDraws)
{
    // Two depth-writing draws in one run: the software renderer sorts them front to back (early-Z). The far one is
    // recorded first with the dim on (the backdrop), the near one after with it off (the stage); the near one covers
    // the frame and must come out undimmed although it is rasterized first.
    const Frame frame = Render([&] {
        renderer.SetStageDim(0.65f);
        renderer.DrawMesh(far, Identity, red);
        renderer.SetStageDim(0.0f);
        renderer.DrawMesh(near, Identity, green);
    });
    EXPECT_EQ(frame.At(32, 32), Green);

    // The other way round: the dimmed draw is the near one
    const Frame other = Render([&] {
        renderer.DrawMesh(far, Identity, green);
        renderer.SetStageDim(0.65f);
        renderer.DrawMesh(near, Identity, red);
        renderer.SetStageDim(0.0f);
    });
    EXPECT_EQ(other.At(32, 32), Dimmed(Red, 0.65f));
}
