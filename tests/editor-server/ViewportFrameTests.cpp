#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// RenderFrame's pixels (#69): EditorServer::ReadFrame turns a renderer's readback (RGBA, bottom row first) into
// FrameData's layout, RGBA with the top row first and alpha 255. Rendered on a headless software renderer at a
// size set the way SetViewportSize sets it (SetRenderTargetSize), so no window or GPU is needed. The commands
// themselves are tested through the application's window with a fake renderer (Window::AdoptRenderer).

using N2Engine::Application;
using N2Engine::Editor::EditorServer;
using N2Engine::Editor::Protocol::BufferReader;
using N2Engine::Editor::Protocol::BufferWriter;
using N2Engine::Editor::CommandType;
using N2Engine::Editor::ResponseType;
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
            if (!renderer.SetRenderTargetSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
            {
                return false;
            }
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

// ============================================================================
// SetViewportSize and RenderFrame, through the application's window with a fake renderer
// ============================================================================

namespace
{
    using Renderer::Common::ITexture;

    /// Records SetRenderTargetSize and fails it on demand; ReadFramebuffer writes each pixel's position as a
    /// backend would give it (row 0 the bottom), so the server's flip shows
    class FakeTargetRenderer final : public Renderer::Common::IRenderer
    {
    public:
        bool acceptTargets = true;
        int targetCalls = 0;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;

        bool SetRenderTargetSize(const uint32_t width, const uint32_t height) override
        {
            ++targetCalls;
            if (!acceptTargets)
            {
                return false;
            }
            targetWidth = width;
            targetHeight = height;
            return true;
        }

        /// R = the row counted from the bottom, G = the column, B = 7, A = 0
        void ReadFramebuffer(std::uint8_t *buffer, const int width, const int height) const override
        {
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    std::uint8_t *pixel = buffer + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                    static_cast<std::size_t>(x)) * 4;
                    pixel[0] = static_cast<std::uint8_t>(y);
                    pixel[1] = static_cast<std::uint8_t>(x);
                    pixel[2] = 7;
                    pixel[3] = 0;
                }
            }
        }

        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return false; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return false; }
        IMesh *CreateMesh(const MeshData &) override { return nullptr; }
        void DestroyMesh(IMesh *) override {}
        ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override { return nullptr; }
        void DestroyTexture(ITexture *) override {}
        IMaterial *CreateMaterial(Renderer::Common::IShader *) override { return nullptr; }
        IMaterial *CreateMaterial(Renderer::Common::IShader *, ITexture *) override { return nullptr; }
        void DestroyMaterial(IMaterial *) override {}
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &,
                                 const N2Engine::Math::Vector3 &) override {}
        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *, const float *, IMaterial *, const Renderer::Common::RenderState &) override {}
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] Renderer::Common::IShader *GetStandardUnlitShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override { return nullptr; }
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "FakeTarget"; }
    };

    struct Response
    {
        std::uint8_t type = 0xEE;
        std::vector<std::uint8_t> body;
    };

    Response Execute(EditorServer &server, const CommandType type, const std::vector<std::uint8_t> &payload = {})
    {
        const std::vector<std::uint8_t> frame = server.ExecuteCommand(static_cast<std::uint8_t>(type), payload);
        BufferReader reader(frame);
        Response response;
        response.type = reader.ReadU8();
        const auto body = reader.ReadBytes(reader.ReadU32());
        response.body.assign(body.begin(), body.end());
        return response;
    }

    Response SetViewportSize(EditorServer &server, const int width, const int height)
    {
        BufferWriter w;
        w.WriteI32(width);
        w.WriteI32(height);
        return Execute(server, CommandType::SetViewportSize, {w.Data().begin(), w.Data().end()});
    }

    std::string Text(const Response &response) { return {response.body.begin(), response.body.end()}; }

    constexpr auto OkType = static_cast<std::uint8_t>(ResponseType::Ok);
    constexpr auto ErrorType = static_cast<std::uint8_t>(ResponseType::Error);
    constexpr auto FrameDataType = static_cast<std::uint8_t>(ResponseType::FrameData);

    /// Gives the application's window a FakeTargetRenderer for the test, and takes it away again
    class ViewportCommandTest : public ::testing::Test
    {
    protected:
        FakeTargetRenderer *fake = nullptr;
        EditorServer server;

        void SetUp() override
        {
            auto renderer = std::make_unique<FakeTargetRenderer>();
            fake = renderer.get();
            Application::GetInstance().GetWindow().AdoptRenderer(std::move(renderer));
        }

        void TearDown() override { Application::GetInstance().GetWindow().AdoptRenderer(nullptr); }
    };
}

TEST_F(ViewportCommandTest, FramesComeAtTheViewportSizeTopRowFirst)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    EXPECT_EQ(fake->targetWidth, 6u);
    EXPECT_EQ(fake->targetHeight, 3u);
    EXPECT_EQ(Application::GetInstance().GetWindow().GetRenderDimensions()[0], 6);
    EXPECT_EQ(Application::GetInstance().GetWindow().GetRenderDimensions()[1], 3);

    const Response frame = Execute(server, CommandType::RenderFrame);
    ASSERT_EQ(frame.type, FrameDataType) << Text(frame);
    BufferReader reader(frame.body);
    EXPECT_EQ(reader.ReadU32(), 6u);
    EXPECT_EQ(reader.ReadU32(), 3u);
    ASSERT_EQ(reader.Remaining(), 6u * 3u * 4u);
    const auto pixels = reader.ReadBytes(reader.Remaining());

    // Row 0 of FrameData is the backend's top row (2, counted from the bottom); alpha is made 255
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 6; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * 6 + static_cast<std::size_t>(x)) * 4;
            EXPECT_EQ(pixels[i], 2 - y) << x << "," << y;
            EXPECT_EQ(pixels[i + 1], x) << x << "," << y;
            EXPECT_EQ(pixels[i + 2], 7) << x << "," << y;
            EXPECT_EQ(pixels[i + 3], 255) << x << "," << y;
        }
    }

    // The same size again doesn't make a new target
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    EXPECT_EQ(Execute(server, CommandType::RenderFrame).type, FrameDataType);
    EXPECT_EQ(fake->targetCalls, 1);
}

TEST_F(ViewportCommandTest, AFailedRenderTargetIsAnErrorAndIsRetried)
{
    fake->acceptTargets = false;

    const Response set = SetViewportSize(server, 8, 4);
    EXPECT_EQ(set.type, ErrorType);
    EXPECT_EQ(Text(set), "Couldn't create a 8x4 render target");
    EXPECT_EQ(Text(set), EditorServer::RenderTargetError(8, 4));
    EXPECT_FALSE(Application::GetInstance().GetWindow().HasRenderSize());

    // RenderFrame never sends a frame of another size: it tries the target again, and fails again
    const Response frame = Execute(server, CommandType::RenderFrame);
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_EQ(Text(frame), "Couldn't create a 8x4 render target");
    EXPECT_EQ(fake->targetCalls, 2);

    // Once the renderer can make it, the same size works without a new SetViewportSize
    fake->acceptTargets = true;
    const Response retried = Execute(server, CommandType::RenderFrame);
    ASSERT_EQ(retried.type, FrameDataType) << Text(retried);
    BufferReader reader(retried.body);
    EXPECT_EQ(reader.ReadU32(), 8u);
    EXPECT_EQ(reader.ReadU32(), 4u);
    EXPECT_EQ(fake->targetCalls, 3);
    EXPECT_EQ(fake->targetWidth, 8u);
    EXPECT_TRUE(Application::GetInstance().GetWindow().HasRenderSize());

    // And SetViewportSize with that size again is Ok, without a new target
    EXPECT_EQ(SetViewportSize(server, 8, 4).type, OkType);
    EXPECT_EQ(fake->targetCalls, 3);
}
