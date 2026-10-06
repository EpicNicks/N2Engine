#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// IRenderer::SetRenderTargetSize on the headless software renderer (#69): frames render at the new size, and
// ReadFramebuffer at that size is an exact copy, RGBA with the bottom row first.

using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::MeshData;
using Renderer::Common::Vertex;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    /// The bottom half of the view in NDC (y -1..0), wound counter-clockwise
    MeshData BottomHalf()
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{1.0f, -1.0f, 0.0f}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{-1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    bool IsRed(const std::vector<std::uint8_t> &rgba, const std::size_t pixel)
    {
        return rgba[pixel * 4] == 255 && rgba[pixel * 4 + 1] == 0 && rgba[pixel * 4 + 2] == 0;
    }

    bool IsBlue(const std::vector<std::uint8_t> &rgba, const std::size_t pixel)
    {
        return rgba[pixel * 4] == 0 && rgba[pixel * 4 + 1] == 0 && rgba[pixel * 4 + 2] == 255;
    }
}

TEST(SoftwareRenderTargetTest, FramesRenderAtTheNewSizeAndReadBackExactly)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 8, 4));
    renderer.Clear(0.0f, 0.0f, 1.0f, 1.0f);
    IMaterial *red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(red, nullptr);
    red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);
    IMesh *bottomHalf = renderer.CreateMesh(BottomHalf());
    ASSERT_NE(bottomHalf, nullptr);

    // Taller and wider than before, and not a multiple of the old size: resampling would show
    constexpr int width = 13;
    constexpr int height = 10;
    ASSERT_TRUE(renderer.SetRenderTargetSize(width, height));

    renderer.BeginFrame();
    renderer.SetViewProjection(Identity, Identity);
    renderer.DrawMesh(bottomHalf, Identity, red);
    renderer.EndFrame();

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    renderer.ReadFramebuffer(rgba.data(), width, height);

    // Bottom row first: the first height / 2 rows are the red bottom half, the rest the blue clear colour
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const auto pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
            if (y < height / 2)
            {
                EXPECT_TRUE(IsRed(rgba, pixel)) << x << "," << y;
            }
            else
            {
                EXPECT_TRUE(IsBlue(rgba, pixel)) << x << "," << y;
            }
        }
    }

    renderer.Shutdown();
}

TEST(SoftwareRenderTargetTest, AZeroDimensionIsIgnored)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4));
    renderer.Clear(0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_FALSE(renderer.SetRenderTargetSize(0, 8));
    EXPECT_FALSE(renderer.SetRenderTargetSize(8, 0));

    renderer.BeginFrame();
    renderer.EndFrame();

    // Still 4x4: reading it at 4x4 is the clear colour throughout
    std::vector<std::uint8_t> rgba(4 * 4 * 4, 7);
    renderer.ReadFramebuffer(rgba.data(), 4, 4);
    for (std::size_t pixel = 0; pixel < 16; ++pixel)
    {
        EXPECT_TRUE(IsBlue(rgba, pixel)) << pixel;
    }

    renderer.Shutdown();
}
