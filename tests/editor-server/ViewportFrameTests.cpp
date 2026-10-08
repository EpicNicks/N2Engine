#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

// ==================== The editor camera and render on demand (#79, E7a) ====================

namespace
{
    constexpr auto FrameUpdateType = static_cast<std::uint8_t>(ResponseType::FrameUpdate);
    constexpr auto EditorCameraType = static_cast<std::uint8_t>(ResponseType::EditorCamera);

    struct CameraArgs
    {
        float position[3] = {0.0f, 0.0f, 10.0f};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
        float fovY = 60.0f;
        bool orthographic = false;
        float orthoSize = 5.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    Response SetCamera(EditorServer &server, const CameraArgs &camera)
    {
        BufferWriter w;
        for (const float v : camera.position)
            w.WriteF32(v);
        for (const float v : camera.rotation)
            w.WriteF32(v);
        w.WriteF32(camera.fovY);
        w.WriteBool(camera.orthographic);
        w.WriteF32(camera.orthoSize);
        w.WriteF32(camera.nearPlane);
        w.WriteF32(camera.farPlane);
        return Execute(server, CommandType::SetEditorCamera, {w.Data().begin(), w.Data().end()});
    }

    struct CameraReply
    {
        float position[3] = {};
        float rotation[4] = {};
        float fovY = 0.0f;
        bool orthographic = false;
        float orthoSize = 0.0f;
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        float view[16] = {};
        float projection[16] = {};
    };

    CameraReply GetCamera(EditorServer &server)
    {
        const Response response = Execute(server, CommandType::GetEditorCamera);
        EXPECT_EQ(response.type, EditorCameraType) << Text(response);
        CameraReply reply;
        if (response.type != EditorCameraType)
            return reply;
        BufferReader r(response.body);
        for (float &v : reply.position)
            v = r.ReadF32();
        for (float &v : reply.rotation)
            v = r.ReadF32();
        reply.fovY = r.ReadF32();
        reply.orthographic = r.ReadBool();
        reply.orthoSize = r.ReadF32();
        reply.nearPlane = r.ReadF32();
        reply.farPlane = r.ReadF32();
        for (float &v : reply.view)
            v = r.ReadF32();
        for (float &v : reply.projection)
            v = r.ReadF32();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after EditorCamera";
        return reply;
    }

    struct FrameReply
    {
        std::uint8_t type = 0xEE;
        std::uint32_t revision = 0;
        bool modified = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;
        std::size_t bodySize = 0;
    };

    FrameReply FrameIfChanged(EditorServer &server, const std::uint32_t sinceRevision)
    {
        BufferWriter w;
        w.WriteU32(sinceRevision);
        const Response response = Execute(server, CommandType::RenderFrameIfChanged, {w.Data().begin(), w.Data().end()});
        FrameReply reply;
        reply.type = response.type;
        reply.bodySize = response.body.size();
        if (response.type != FrameUpdateType)
        {
            ADD_FAILURE() << "not a FrameUpdate: " << Text(response);
            return reply;
        }
        BufferReader r(response.body);
        reply.revision = r.ReadU32();
        reply.modified = r.ReadBool();
        reply.width = r.ReadU32();
        reply.height = r.ReadU32();
        const auto pixels = r.ReadBytes(r.Remaining());
        reply.pixels.assign(pixels.begin(), pixels.end());
        return reply;
    }

    /// The kinds of the events pushed after `afterSeq`
    std::vector<std::string> EventKindsAfter(const EditorServer &server, const std::uint32_t afterSeq)
    {
        std::vector<std::string> kinds;
        for (const nlohmann::json &event : server.GetEvents().Read(afterSeq, 4096, 0).events)
        {
            kinds.push_back(event.value("kind", ""));
        }
        return kinds;
    }

    std::size_t CountKind(const std::vector<std::string> &kinds, const std::string &kind)
    {
        return static_cast<std::size_t>(std::count(kinds.begin(), kinds.end(), kind));
    }
}

TEST_F(ViewportCommandTest, TheEditorCameraStartsAtItsDefaultsAndEchoesWhatWasSet)
{
    const CameraReply initial = GetCamera(server);
    EXPECT_EQ(initial.position[2], 10.0f);
    EXPECT_EQ(initial.rotation[3], 1.0f);
    EXPECT_EQ(initial.fovY, 60.0f);
    EXPECT_FALSE(initial.orthographic);
    EXPECT_EQ(initial.nearPlane, 0.1f);
    EXPECT_EQ(initial.farPlane, 1000.0f);

    CameraArgs camera;
    camera.position[0] = 1.5f;
    camera.position[1] = -2.0f;
    camera.position[2] = 30.0f;
    camera.fovY = 75.0f;
    camera.nearPlane = 0.5f;
    camera.farPlane = 500.0f;
    camera.rotation[0] = 0.0f;
    camera.rotation[1] = 2.0f; // not a unit quaternion: stored normalised
    camera.rotation[2] = 0.0f;
    camera.rotation[3] = 0.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);

    const CameraReply reply = GetCamera(server);
    EXPECT_EQ(reply.position[0], 1.5f);
    EXPECT_EQ(reply.position[1], -2.0f);
    EXPECT_EQ(reply.position[2], 30.0f);
    EXPECT_EQ(reply.fovY, 75.0f);
    EXPECT_EQ(reply.nearPlane, 0.5f);
    EXPECT_EQ(reply.farPlane, 500.0f);
    EXPECT_NEAR(reply.rotation[1], 1.0f, 1e-6f);
    EXPECT_NEAR(reply.rotation[3], 0.0f, 1e-6f);

    // The game's main camera is a separate thing: SetEditorCamera never moved it
    if (const auto *mainCamera = Application::GetInstance().GetMainCamera())
    {
        EXPECT_NE(mainCamera->GetPosition()[2], 30.0f);
    }
}

TEST_F(ViewportCommandTest, TheEditorCamerasMatricesFollowItsPoseAndTheViewport)
{
    ASSERT_EQ(SetViewportSize(server, 200, 100).type, OkType); // aspect 2
    CameraArgs camera;
    camera.position[0] = 3.0f;
    camera.position[1] = 4.0f;
    camera.position[2] = 5.0f;
    camera.fovY = 90.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);

    const CameraReply reply = GetCamera(server);
    // Column-major, for column vectors: the translation of the view is elements 12 to 14. A camera with no rotation
    // at (3, 4, 5) moves the world by (-3, -4, -5)
    EXPECT_NEAR(reply.view[0], 1.0f, 1e-5f);
    EXPECT_NEAR(reply.view[5], 1.0f, 1e-5f);
    EXPECT_NEAR(reply.view[10], 1.0f, 1e-5f);
    EXPECT_NEAR(reply.view[12], -3.0f, 1e-5f);
    EXPECT_NEAR(reply.view[13], -4.0f, 1e-5f);
    EXPECT_NEAR(reply.view[14], -5.0f, 1e-5f);
    // Perspective with a 90 degree vertical field of view: 1 / (aspect * tan 45), 1 / tan 45, and w = -z
    EXPECT_NEAR(reply.projection[0], 0.5f, 1e-3f);
    EXPECT_NEAR(reply.projection[5], 1.0f, 1e-3f);
    EXPECT_NEAR(reply.projection[11], -1.0f, 1e-6f);

    // Another viewport shape changes the projection and nothing else
    ASSERT_EQ(SetViewportSize(server, 100, 100).type, OkType);
    const CameraReply square = GetCamera(server);
    EXPECT_NEAR(square.projection[0], 1.0f, 1e-3f);
    EXPECT_NEAR(square.view[12], -3.0f, 1e-5f);

    // Orthographic: the half height is orthoSize, the half width orthoSize * aspect, and w stays 1
    camera.orthographic = true;
    camera.orthoSize = 4.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);
    ASSERT_EQ(SetViewportSize(server, 200, 100).type, OkType);
    const CameraReply ortho = GetCamera(server);
    EXPECT_TRUE(ortho.orthographic);
    EXPECT_NEAR(ortho.projection[0], 1.0f / 8.0f, 1e-5f);
    EXPECT_NEAR(ortho.projection[5], 1.0f / 4.0f, 1e-5f);
    EXPECT_NEAR(ortho.projection[15], 1.0f, 1e-6f);
}

TEST_F(ViewportCommandTest, ARefusedEditorCameraChangesNothing)
{
    CameraArgs good;
    good.position[0] = 7.0f;
    ASSERT_EQ(SetCamera(server, good).type, OkType);
    const std::uint32_t revision = server.GetFrameRevision();
    const std::uint32_t seq = server.GetEvents().LastSeq();

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<CameraArgs> bad;
    const auto add = [&](auto change)
    {
        CameraArgs camera = good;
        change(camera);
        bad.push_back(camera);
    };
    add([&](CameraArgs &c) { c.position[0] = nan; });
    add([&](CameraArgs &c) { c.position[1] = inf; });
    add([&](CameraArgs &c) { c.position[2] = -inf; });
    add([&](CameraArgs &c) { c.position[2] = 1.0e9f; });
    add([&](CameraArgs &c) { c.rotation[0] = nan; });
    add([&](CameraArgs &c) { c.rotation[3] = inf; });
    add([&](CameraArgs &c) { c.rotation[0] = c.rotation[1] = c.rotation[2] = c.rotation[3] = 0.0f; });
    add([&](CameraArgs &c) { c.rotation[3] = 3.0e38f; c.rotation[0] = 3.0e38f; }); // too large to normalise
    add([&](CameraArgs &c) { c.fovY = nan; });
    add([&](CameraArgs &c) { c.fovY = 0.0f; });
    add([&](CameraArgs &c) { c.fovY = 180.0f; });
    add([&](CameraArgs &c) { c.fovY = -60.0f; });
    add([&](CameraArgs &c) { c.nearPlane = 0.0f; });
    add([&](CameraArgs &c) { c.nearPlane = -1.0f; });
    add([&](CameraArgs &c) { c.nearPlane = nan; });
    add([&](CameraArgs &c) { c.farPlane = c.nearPlane; });
    add([&](CameraArgs &c) { c.farPlane = c.nearPlane - 1.0f; });
    add([&](CameraArgs &c) { c.farPlane = inf; });
    add([&](CameraArgs &c) { c.farPlane = 1.0e9f; });
    add([&](CameraArgs &c) { c.orthographic = true; c.orthoSize = 0.0f; });
    add([&](CameraArgs &c) { c.orthographic = true; c.orthoSize = -2.0f; });
    add([&](CameraArgs &c) { c.orthographic = true; c.orthoSize = nan; });
    add([&](CameraArgs &c) { c.orthographic = true; c.orthoSize = 1.0e9f; });
    add([&](CameraArgs &c) { c.orthoSize = nan; }); // not in use, but still not a number
    for (std::size_t i = 0; i < bad.size(); ++i)
    {
        const Response response = SetCamera(server, bad[i]);
        EXPECT_EQ(response.type, ErrorType) << "case " << i;
        EXPECT_FALSE(Text(response).empty()) << "case " << i;
    }

    const CameraReply after = GetCamera(server);
    EXPECT_EQ(after.position[0], 7.0f);
    EXPECT_EQ(server.GetFrameRevision(), revision) << "a refused camera is no change";
    EXPECT_TRUE(EventKindsAfter(server, seq).empty()) << "and nothing is logged or announced for it";

    // A request that is cut short is an Error too, and changes nothing
    EXPECT_EQ(Execute(server, CommandType::SetEditorCamera, std::vector<std::uint8_t>(10, 0)).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::SetEditorCamera).type, ErrorType);
    EXPECT_EQ(GetCamera(server).position[0], 7.0f);
}

TEST_F(ViewportCommandTest, AnOrthographicCameraMayHaveANearPlaneBehindIt)
{
    CameraArgs camera;
    camera.orthographic = true;
    camera.orthoSize = 3.0f;
    camera.nearPlane = -50.0f;
    camera.farPlane = 50.0f;
    EXPECT_EQ(SetCamera(server, camera).type, OkType);
    // A perspective camera can't
    camera.orthographic = false;
    EXPECT_EQ(SetCamera(server, camera).type, ErrorType);
}

TEST_F(ViewportCommandTest, SettingTheCameraItAlreadyHasIsNotAChange)
{
    CameraArgs camera;
    camera.position[1] = 2.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);
    const std::uint32_t revision = server.GetFrameRevision();
    const std::uint32_t seq = server.GetEvents().LastSeq();

    EXPECT_EQ(SetCamera(server, camera).type, OkType);
    EXPECT_EQ(server.GetFrameRevision(), revision);
    EXPECT_TRUE(EventKindsAfter(server, seq).empty());

    camera.position[1] = 3.0f;
    EXPECT_EQ(SetCamera(server, camera).type, OkType);
    EXPECT_NE(server.GetFrameRevision(), revision);
}

TEST_F(ViewportCommandTest, AFrameIsRenderedOnlyWhenWhatItShowsChanged)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);

    // A client with no frame (0) gets one, at the viewport size, laid out as RenderFrame's
    const FrameReply first = FrameIfChanged(server, 0);
    ASSERT_EQ(first.type, FrameUpdateType);
    EXPECT_TRUE(first.modified);
    EXPECT_NE(first.revision, 0u);
    EXPECT_EQ(first.revision, server.GetFrameRevision());
    EXPECT_EQ(first.width, 6u);
    EXPECT_EQ(first.height, 3u);
    ASSERT_EQ(first.pixels.size(), 6u * 3u * 4u);
    EXPECT_EQ(first.pixels[0], 2) << "the top row first";
    EXPECT_EQ(first.pixels[3], 255) << "opaque";
    EXPECT_EQ(server.GetEditorFramesRendered(), 1u);

    // The client holds the current frame: no pixels, and nothing rendered
    for (int i = 0; i < 3; ++i)
    {
        const FrameReply again = FrameIfChanged(server, first.revision);
        EXPECT_FALSE(again.modified);
        EXPECT_EQ(again.revision, first.revision);
        EXPECT_EQ(again.width, 6u);
        EXPECT_EQ(again.height, 3u);
        EXPECT_TRUE(again.pixels.empty());
        EXPECT_EQ(again.bodySize, 13u) << "revision, modified, width, height";
    }
    EXPECT_EQ(server.GetEditorFramesRendered(), 1u);

    // A client that lost its copy asks with 0, and is sent the one in the buffer without rendering again
    const FrameReply resent = FrameIfChanged(server, 0);
    EXPECT_TRUE(resent.modified);
    EXPECT_EQ(resent.revision, first.revision);
    EXPECT_EQ(resent.pixels, first.pixels);
    EXPECT_EQ(server.GetEditorFramesRendered(), 1u);

    // A revision the server never handed out (a stale one from another host) is not current either
    EXPECT_TRUE(FrameIfChanged(server, first.revision + 12345u).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), 1u);

    // The camera moves: the next frame is rendered once, whoever asks
    CameraArgs camera;
    camera.position[0] = 4.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);
    const FrameReply moved = FrameIfChanged(server, first.revision);
    EXPECT_TRUE(moved.modified);
    EXPECT_NE(moved.revision, first.revision);
    EXPECT_EQ(server.GetEditorFramesRendered(), 2u);
    EXPECT_FALSE(FrameIfChanged(server, moved.revision).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), 2u);
}

TEST_F(ViewportCommandTest, ANewViewportSizeIsANewFrameAndTheSameSizeIsNot)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    const FrameReply first = FrameIfChanged(server, 0);
    ASSERT_TRUE(first.modified);

    // A client sends the size on every resize event, and most repeat it
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    EXPECT_FALSE(FrameIfChanged(server, first.revision).modified);

    ASSERT_EQ(SetViewportSize(server, 8, 4).type, OkType);
    const FrameReply resized = FrameIfChanged(server, first.revision);
    EXPECT_TRUE(resized.modified);
    EXPECT_EQ(resized.width, 8u);
    EXPECT_EQ(resized.height, 4u);
    EXPECT_EQ(resized.pixels.size(), 8u * 4u * 4u);

    // A refused size changes nothing
    EXPECT_EQ(SetViewportSize(server, 0, 4).type, ErrorType);
    EXPECT_FALSE(FrameIfChanged(server, resized.revision).modified);
}

TEST_F(ViewportCommandTest, RenderFrameLeavesTheEditorViewsRevisionAlone)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    const FrameReply first = FrameIfChanged(server, 0);
    ASSERT_TRUE(first.modified);
    const std::uint32_t revision = server.GetFrameRevision();

    // The game camera's frame goes through the same buffer
    ASSERT_EQ(Execute(server, CommandType::RenderFrame).type, FrameDataType);
    EXPECT_EQ(server.GetFrameRevision(), revision);

    // A client holding the editor frame still holds the current one
    EXPECT_FALSE(FrameIfChanged(server, first.revision).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), 1u);

    // A client without one is sent the editor view, rendered again since the buffer held the game camera's picture
    const FrameReply again = FrameIfChanged(server, 0);
    EXPECT_TRUE(again.modified);
    EXPECT_EQ(again.revision, revision);
    EXPECT_EQ(server.GetEditorFramesRendered(), 2u);
}

TEST_F(ViewportCommandTest, FrameChangedIsPushedOncePerChangeAfterAFrame)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    const FrameReply first = FrameIfChanged(server, 0);
    ASSERT_TRUE(first.modified);

    // Nothing changed: nothing is announced, however much the client polls
    const std::uint32_t quiet = server.GetEvents().LastSeq();
    for (int i = 0; i < 5; ++i)
    {
        (void)FrameIfChanged(server, first.revision);
        (void)GetCamera(server);
    }
    EXPECT_TRUE(EventKindsAfter(server, quiet).empty());

    // A drag of the camera: one event, not one per step
    const std::uint32_t before = server.GetEvents().LastSeq();
    CameraArgs camera;
    for (int step = 1; step <= 6; ++step)
    {
        camera.position[0] = static_cast<float>(step);
        ASSERT_EQ(SetCamera(server, camera).type, OkType);
    }
    const std::vector<std::string> kinds = EventKindsAfter(server, before);
    EXPECT_EQ(CountKind(kinds, "frameChanged"), 1u);
    EXPECT_EQ(kinds.size(), 1u) << "and no log line for any of the steps";
    const nlohmann::json event = server.GetEvents().Read(before, 10, 0).events.front();
    EXPECT_EQ(event.at("kind"), "frameChanged");
    EXPECT_NE(event.at("revision").get<std::uint32_t>(), first.revision);

    // The frame is rendered, and the next change is announced again
    const FrameReply second = FrameIfChanged(server, first.revision);
    ASSERT_TRUE(second.modified);
    const std::uint32_t afterFrame = server.GetEvents().LastSeq();
    camera.position[0] = 20.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);
    EXPECT_EQ(CountKind(EventKindsAfter(server, afterFrame), "frameChanged"), 1u);
}

TEST_F(ViewportCommandTest, ARenderRequestWithoutARendererIsAnError)
{
    Application::GetInstance().GetWindow().AdoptRenderer(nullptr);
    const FrameReply none = [&]
    {
        BufferWriter w;
        w.WriteU32(0);
        const Response response = Execute(server, CommandType::RenderFrameIfChanged, {w.Data().begin(), w.Data().end()});
        FrameReply reply;
        reply.type = response.type;
        return reply;
    }();
    EXPECT_EQ(none.type, ErrorType);
    EXPECT_EQ(server.GetEditorFramesRendered(), 0u);
    // The camera commands don't need a renderer
    EXPECT_EQ(GetCamera(server).fovY, 60.0f);
}

TEST_F(ViewportCommandTest, ARefusedRenderTargetIsAnErrorAndRetriedWithoutRenderingAnything)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    fake->acceptTargets = false;
    ASSERT_EQ(SetViewportSize(server, 8, 4).type, ErrorType);

    BufferWriter ask;
    ask.WriteU32(0);
    const Response refused = Execute(server, CommandType::RenderFrameIfChanged, {ask.Data().begin(), ask.Data().end()});
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_EQ(Text(refused), EditorServer::RenderTargetError(8, 4));
    EXPECT_EQ(server.GetEditorFramesRendered(), 0u);

    fake->acceptTargets = true;
    const FrameReply retried = FrameIfChanged(server, 0);
    ASSERT_EQ(retried.type, FrameUpdateType);
    EXPECT_EQ(retried.width, 8u);
    EXPECT_EQ(retried.height, 4u);
}

TEST_F(ViewportCommandTest, ATruncatedRenderFrameIfChangedIsAnError)
{
    EXPECT_EQ(Execute(server, CommandType::RenderFrameIfChanged).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::RenderFrameIfChanged, std::vector<std::uint8_t>(3, 0)).type, ErrorType);
}

TEST(EditorServerPolledCommandsTest, TheViewportCommandsAreQuiet)
{
    for (const CommandType type : {CommandType::RenderFrame, CommandType::RenderFrameIfChanged,
                                   CommandType::SetEditorCamera, CommandType::GetEditorCamera})
    {
        EXPECT_TRUE(EditorServer::IsPolledCommand(static_cast<std::uint8_t>(type))) << static_cast<int>(type);
    }
}

TEST_F(ViewportCommandTest, FrameChangedIsAnnouncedAgainAfterARenderThatFailed)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    ASSERT_TRUE(FrameIfChanged(server, 0).modified);

    const std::uint32_t before = server.GetEvents().LastSeq();
    fake->acceptTargets = false;
    ASSERT_EQ(SetViewportSize(server, 8, 4).type, ErrorType); // a change: announced
    CameraArgs camera;
    camera.position[0] = 1.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType); // not announced again: no frame since
    EXPECT_EQ(CountKind(EventKindsAfter(server, before), "frameChanged"), 1u);

    // The client asks, and the render fails: the announcement didn't lead to a frame
    BufferWriter ask;
    ask.WriteU32(0);
    EXPECT_EQ(Execute(server, CommandType::RenderFrameIfChanged, {ask.Data().begin(), ask.Data().end()}).type, ErrorType);

    camera.position[0] = 2.0f;
    ASSERT_EQ(SetCamera(server, camera).type, OkType);
    EXPECT_EQ(CountKind(EventKindsAfter(server, before), "frameChanged"), 2u) << "so the next change says so again";
}

TEST_F(ViewportCommandTest, RenderFrameMarksTheBufferStaleBeforeItDraws)
{
    ASSERT_EQ(SetViewportSize(server, 6, 3).type, OkType);
    ASSERT_TRUE(FrameIfChanged(server, 0).modified);
    ASSERT_EQ(server.GetEditorFramesRendered(), 1u);

    // The game camera's frame uses the buffer; a client with no frame then gets the editor view rendered afresh
    ASSERT_EQ(Execute(server, CommandType::RenderFrame).type, FrameDataType);
    ASSERT_TRUE(FrameIfChanged(server, 0).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), 2u);
}
