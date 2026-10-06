#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <editor-server/EditorServer.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// RenderFrame's pixels (#69): EditorServer::ReadFrame turns a renderer's readback (RGBA, bottom row first) into
// FrameData's layout, RGBA with the top row first and alpha 255. Rendered on a headless software renderer at a
// size set the way SetViewportSize sets it (SetRenderTargetSize), so no window or GPU is needed.

using N2Engine::Editor::EditorServer;
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

    struct Rgba
    {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
        bool operator==(const Rgba &) const = default;
    };

    /// y counts down from the top row, as in an image
    Rgba At(const std::vector<std::uint8_t> &pixels, const int width, const int x, const int y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x)) * 4;
        return Rgba{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    /// A headless software renderer with a frame drawn: a half-transparent blue clear colour, and the bottom
    /// half of the view red
    struct BottomHalfRedFrame
    {
        SoftwareRenderer renderer;

        bool Render(const int width, const int height)
        {
            // Started at another size, then sized as SetViewportSize does it
            if (!renderer.Initialize(nullptr, 16, 16))
            {
                return false;
            }
            renderer.SetRenderTargetSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            renderer.Clear(0.0f, 0.0f, 1.0f, 0.5f);
            IMaterial *red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            IMesh *bottomHalf = renderer.CreateMesh(BottomHalf());
            if (!red || !bottomHalf)
            {
                return false;
            }
            red->SetColor("uAlbedo", 1.0f, 0.0f, 0.0f, 1.0f);

            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            renderer.DrawMesh(bottomHalf, Identity, red);
            renderer.EndFrame();
            return true;
        }

        ~BottomHalfRedFrame() { renderer.Shutdown(); }
    };
}

TEST(ViewportFrameTest, PixelsAreRgbaTopRowFirstAtTheViewportSize)
{
    constexpr int width = 20;
    constexpr int height = 12;
    BottomHalfRedFrame frame;
    ASSERT_TRUE(frame.Render(width, height));

    std::vector<std::uint8_t> pixels;
    EditorServer::ReadFrame(frame.renderer, width, height, pixels);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(width) * height * 4);

    // The top half of the image (the first rows) is the clear colour, the bottom half red: upright, in RGBA order
    constexpr Rgba blue{0, 0, 255, 255};
    constexpr Rgba red{255, 0, 0, 255};
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            EXPECT_EQ(At(pixels, width, x, y), y < height / 2 ? blue : red) << x << "," << y;
        }
    }
}

TEST(ViewportFrameTest, AlphaIsOpaqueWhateverTheTargetHolds)
{
    // The clear colour's alpha is 0.5: the readback has ~127 there, FrameData always 255
    BottomHalfRedFrame frame;
    ASSERT_TRUE(frame.Render(4, 4));

    std::vector<std::uint8_t> raw(4 * 4 * 4);
    frame.renderer.ReadFramebuffer(raw.data(), 4, 4);
    ASSERT_LT(raw[(3 * 4) * 4 + 3], 255) << "the top-left pixel's alpha should be the clear colour's";

    std::vector<std::uint8_t> pixels;
    EditorServer::ReadFrame(frame.renderer, 4, 4, pixels);
    for (std::size_t i = 3; i < pixels.size(); i += 4)
    {
        EXPECT_EQ(pixels[i], 255) << "pixel " << i / 4;
    }
}

TEST(ViewportFrameTest, ReplacesWhateverThePixelBufferHeld)
{
    BottomHalfRedFrame frame;
    ASSERT_TRUE(frame.Render(4, 2));

    // A buffer left from a larger frame, and one from a smaller frame, both end up exactly one frame long
    std::vector<std::uint8_t> larger(1000, 7);
    EditorServer::ReadFrame(frame.renderer, 4, 2, larger);
    EXPECT_EQ(larger.size(), 4u * 2u * 4u);
    std::vector<std::uint8_t> smaller(3, 7);
    EditorServer::ReadFrame(frame.renderer, 4, 2, smaller);
    EXPECT_EQ(smaller, larger);

    std::vector<std::uint8_t> none(5, 7);
    EditorServer::ReadFrame(frame.renderer, 0, 2, none);
    EXPECT_TRUE(none.empty());
}
