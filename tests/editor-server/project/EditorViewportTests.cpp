#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <engine/Window.hpp>
#include <engine/common/Color.hpp>
#include <engine/config/ApplicationOptions.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/rendering/Material.hpp>
#include <engine/rendering/Mesh.hpp>
#include <engine/rendering/MeshRenderer.hpp>
#include <engine/rendering/RenderSettings.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// The editor camera and render on demand (#79, E7a) through EditorServer::ExecuteCommand, on a scene made with
// NewScene and a real headless software renderer: the frames are real pixels, so a scene with no camera of its own is
// seen from the editor camera, and the frame revision follows the scene, as the commands change it. Pixels are
// asserted with wide tolerances (white at the middle of a face, the clear colour away from the object), not
// bit for bit, so the tests hold across compilers and build types.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t FrameUpdateType = static_cast<uint8_t>(ResponseType::FrameUpdate);
    constexpr uint8_t EditorCameraType = static_cast<uint8_t>(ResponseType::EditorCamera);

    constexpr int ViewWidth = 64;
    constexpr int ViewHeight = 48;

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        return out;
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    struct Rgb
    {
        int r = 0;
        int g = 0;
        int b = 0;
    };

    /// The built-in shapes are unlit and white until they are given a colour
    bool IsCube(const Rgb &c) { return c.r >= 200 && c.g >= 200 && c.b >= 200; }
    bool IsBlue(const Rgb &c) { return c.b >= 200 && c.r <= 60 && c.g <= 60; }

    struct Picture
    {
        uint8_t type = 0xEE;
        uint32_t revision = 0;
        bool modified = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;

        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            return Rgb{pixels.at(i), pixels.at(i + 1), pixels.at(i + 2)};
        }
        [[nodiscard]] uint8_t Alpha(const int x, const int y) const
        {
            return pixels.at((static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4 + 3);
        }
        [[nodiscard]] Rgb Centre() const { return At(static_cast<int>(width) / 2, static_cast<int>(height) / 2); }
    };

    struct CameraArgs
    {
        std::array<float, 3> position{0.0f, 0.0f, 3.0f};
        std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
        float fovY = 60.0f;
        bool orthographic = false;
        float orthoSize = 5.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    /// A scene made by NewScene on a headless software renderer, viewed at ViewWidth x ViewHeight with a blue clear
    /// colour. The host owns its window, so this gives the application's one the renderer a host would.
    class EditorViewportTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Config::ApplicationOptions options;
            options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            window.clearColor = Common::Color::Blue;

            BufferWriter size;
            size.WriteI32(ViewWidth);
            size.WriteI32(ViewHeight);
            ASSERT_EQ(Execute(server, CommandType::SetViewportSize, size.Release()).type, OkType);

            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Viewport Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            SetCamera(CameraArgs{});
        }

        void TearDown() override
        {
            // The scene's shapes release their GPU resources while the renderer is still alive
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            Application::GetInstance().GetWindow().Shutdown();
        }

        Frame SetCamera(const CameraArgs &camera)
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
            const Frame response = Execute(server, CommandType::SetEditorCamera, w.Release());
            EXPECT_EQ(response.type, OkType) << response.Text();
            return response;
        }

        Picture FrameSince(const uint32_t sinceRevision)
        {
            BufferWriter w;
            w.WriteU32(sinceRevision);
            const Frame response = Execute(server, CommandType::RenderFrameIfChanged, w.Release());
            Picture picture;
            picture.type = response.type;
            if (response.type != FrameUpdateType)
            {
                ADD_FAILURE() << "not a FrameUpdate: " << response.Text();
                return picture;
            }
            BufferReader r(response.payload);
            picture.revision = r.ReadU32();
            picture.modified = r.ReadBool();
            picture.width = r.ReadU32();
            picture.height = r.ReadU32();
            const auto pixels = r.ReadBytes(r.Remaining());
            picture.pixels.assign(pixels.begin(), pixels.end());
            return picture;
        }

        /// A frame whatever the client holds
        Picture Frame0() { return FrameSince(0); }

        std::string Create(const std::string &preset)
        {
            BufferWriter w;
            w.WriteString(preset);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString(preset);
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame SetLocal(const std::string &id, const std::array<float, 3> &position)
        {
            BufferWriter w;
            w.WriteString(id);
            for (const float v : position)
                w.WriteF32(v);
            for (const float v : {0.0f, 0.0f, 0.0f, 1.0f})
                w.WriteF32(v);
            for (const float v : {1.0f, 1.0f, 1.0f})
                w.WriteF32(v);
            return Execute(server, CommandType::SetLocalTransform, w.Release());
        }

        EditorServer server;
    };
}

// ==================== A scene with no camera ====================

TEST_F(EditorViewportTest, ASceneWithNoCameraIsSeenFromTheEditorCamera)
{
    // The scene has no camera object at all, and the game's main camera isn't involved: only the editor camera
    ASSERT_FALSE(Create("Cube").empty());

    const Picture picture = Frame0();
    ASSERT_EQ(picture.type, FrameUpdateType);
    EXPECT_TRUE(picture.modified);
    ASSERT_EQ(picture.width, static_cast<uint32_t>(ViewWidth));
    ASSERT_EQ(picture.height, static_cast<uint32_t>(ViewHeight));
    ASSERT_EQ(picture.pixels.size(), static_cast<std::size_t>(ViewWidth) * ViewHeight * 4);

    // The cube is the middle of the view (an unlit white face); the corners are the clear colour
    EXPECT_TRUE(IsCube(picture.Centre())) << picture.Centre().r << "," << picture.Centre().g << "," << picture.Centre().b;
    EXPECT_TRUE(IsBlue(picture.At(2, 2)));
    EXPECT_TRUE(IsBlue(picture.At(ViewWidth - 3, 2)));
    EXPECT_TRUE(IsBlue(picture.At(2, ViewHeight - 3)));
    EXPECT_TRUE(IsBlue(picture.At(ViewWidth - 3, ViewHeight - 3)));
    // Opaque everywhere
    EXPECT_EQ(picture.Alpha(2, 2), 255);
    EXPECT_EQ(picture.Alpha(ViewWidth / 2, ViewHeight / 2), 255);
}

TEST_F(EditorViewportTest, AnEmptySceneIsTheClearColour)
{
    const Picture picture = Frame0();
    ASSERT_EQ(picture.type, FrameUpdateType);
    for (const auto &[x, y] : {std::pair{2, 2}, std::pair{32, 24}, std::pair{61, 45}})
    {
        EXPECT_TRUE(IsBlue(picture.At(x, y))) << x << "," << y;
    }
}

TEST_F(EditorViewportTest, TheCameraDecidesWhatIsSeen)
{
    ASSERT_FALSE(Create("Cube").empty());
    EXPECT_TRUE(IsCube(Frame0().Centre()));

    // Off to the side, looking straight ahead: the cube is out of view
    CameraArgs aside;
    aside.position = {50.0f, 0.0f, 3.0f};
    SetCamera(aside);
    EXPECT_TRUE(IsBlue(Frame0().Centre()));

    // Behind it, or turned away (a half turn about Y): out of view as well
    CameraArgs turned;
    turned.rotation = {0.0f, 1.0f, 0.0f, 0.0f};
    SetCamera(turned);
    EXPECT_TRUE(IsBlue(Frame0().Centre()));

    // Back, a little further away: smaller but still in the middle
    CameraArgs afar;
    afar.position = {0.0f, 0.0f, 8.0f};
    SetCamera(afar);
    const Picture distant = Frame0();
    EXPECT_TRUE(IsCube(distant.Centre()));
    EXPECT_TRUE(IsBlue(distant.At(14, 24))) << "a small cube leaves the sides clear";

    // Close up, the cube fills more of the view than it did from afar
    CameraArgs closeUp;
    closeUp.position = {0.0f, 0.0f, 1.6f};
    SetCamera(closeUp);
    const Picture close = Frame0();
    EXPECT_TRUE(IsCube(close.Centre()));
    EXPECT_TRUE(IsCube(close.At(22, 24))) << "the face reaches further out";
}

TEST_F(EditorViewportTest, AnOrthographicCameraDrawsTheCubeAtItsSize)
{
    ASSERT_FALSE(Create("Cube").empty());
    CameraArgs ortho;
    ortho.orthographic = true;
    ortho.orthoSize = 1.0f; // half height 1, half width 1.333: the unit cube covers its middle half
    SetCamera(ortho);

    const Picture picture = Frame0();
    ASSERT_EQ(picture.type, FrameUpdateType);
    EXPECT_TRUE(IsCube(picture.Centre()));
    EXPECT_TRUE(IsCube(picture.At(24, 16))) << "inside the face";
    EXPECT_TRUE(IsCube(picture.At(40, 32))) << "inside the face";
    EXPECT_TRUE(IsBlue(picture.At(10, 24))) << "left of it";
    EXPECT_TRUE(IsBlue(picture.At(54, 24))) << "right of it";
    EXPECT_TRUE(IsBlue(picture.At(32, 6))) << "above it";
    EXPECT_TRUE(IsBlue(picture.At(32, 42))) << "below it";

    // The orthographic size is a world size: twice as big, the cube is half as wide on the screen
    ortho.orthoSize = 2.0f;
    SetCamera(ortho);
    const Picture wide = Frame0();
    EXPECT_TRUE(IsCube(wide.Centre()));
    EXPECT_TRUE(IsBlue(wide.At(24, 16))) << "now outside the smaller face";
}

// ==================== The picture follows the scene ====================

TEST_F(EditorViewportTest, ACommandThatChangesTheSceneMakesTheNextFrame)
{
    const Picture empty = Frame0();
    ASSERT_EQ(empty.type, FrameUpdateType);
    EXPECT_TRUE(IsBlue(empty.Centre()));
    const uint32_t emptyRevision = empty.revision;
    const uint32_t rendered = server.GetEditorFramesRendered();

    // Nothing changed: not rendered again
    EXPECT_FALSE(FrameSince(emptyRevision).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), rendered);

    const std::string cube = Create("Cube");
    ASSERT_FALSE(cube.empty());
    const Picture withCube = FrameSince(emptyRevision);
    EXPECT_TRUE(withCube.modified);
    EXPECT_NE(withCube.revision, emptyRevision);
    EXPECT_TRUE(IsCube(withCube.Centre()));
    EXPECT_EQ(server.GetEditorFramesRendered(), rendered + 1);

    // Moving it out of the way
    ASSERT_EQ(SetLocal(cube, {50.0f, 0.0f, 0.0f}).type, OkType);
    const Picture moved = FrameSince(withCube.revision);
    EXPECT_TRUE(moved.modified);
    EXPECT_TRUE(IsBlue(moved.Centre()));

    // And an undo brings it back: the step is a change like any other
    const Frame undone = Execute(server, CommandType::Undo);
    ASSERT_EQ(undone.type, EditResultType) << undone.Text();
    const Picture back = FrameSince(moved.revision);
    EXPECT_TRUE(back.modified);
    EXPECT_TRUE(IsCube(back.Centre()));
    const Frame redone = Execute(server, CommandType::Redo);
    ASSERT_EQ(redone.type, EditResultType) << redone.Text();
    EXPECT_TRUE(IsBlue(FrameSince(back.revision).Centre()));

    // Destroying it
    BufferWriter destroy;
    destroy.WriteString(cube);
    const Frame destroyed = Execute(server, CommandType::DestroyEntity, destroy.Release());
    ASSERT_NE(destroyed.type, ErrorType) << destroyed.Text();
    EXPECT_TRUE(IsBlue(Frame0().Centre()));
}

TEST_F(EditorViewportTest, ReadingAndNoOpsMakeNoNewFrame)
{
    const std::string cube = Create("Cube");
    ASSERT_FALSE(cube.empty());
    ASSERT_EQ(SetLocal(cube, {0.0f, 0.0f, 0.0f}).type, OkType);
    const Picture first = Frame0();
    const uint32_t rendered = server.GetEditorFramesRendered();

    // Everything a client reads while it shows the scene
    (void)Execute(server, CommandType::GetHierarchy);
    BufferWriter entity;
    entity.WriteString(cube);
    (void)Execute(server, CommandType::GetEntity, entity.Release());
    (void)Execute(server, CommandType::GetEngineHealth);
    (void)Execute(server, CommandType::GetEditorCamera);
    (void)Execute(server, CommandType::GetCameraPosition);
    (void)Execute(server, CommandType::GetAllEntities);
    // A transform it already has, and the camera it already has
    ASSERT_EQ(SetLocal(cube, {0.0f, 0.0f, 0.0f}).type, OkType);
    SetCamera(CameraArgs{});
    // The same viewport size again
    BufferWriter size;
    size.WriteI32(ViewWidth);
    size.WriteI32(ViewHeight);
    ASSERT_EQ(Execute(server, CommandType::SetViewportSize, size.Release()).type, OkType);

    EXPECT_FALSE(FrameSince(first.revision).modified);
    EXPECT_EQ(server.GetEditorFramesRendered(), rendered);
}

TEST_F(EditorViewportTest, AnIdleViewportCostsNoRendering)
{
    ASSERT_FALSE(Create("Cube").empty());
    const Picture first = Frame0();
    const uint32_t rendered = server.GetEditorFramesRendered();
    const uint32_t seq = server.GetEvents().LastSeq();

    // A client polling every animation frame for a minute
    for (int i = 0; i < 3600; ++i)
    {
        const Picture poll = FrameSince(first.revision);
        ASSERT_FALSE(poll.modified);
        ASSERT_TRUE(poll.pixels.empty());
    }
    EXPECT_EQ(server.GetEditorFramesRendered(), rendered);
    EXPECT_EQ(server.GetEvents().LastSeq(), seq) << "and none of it is logged or announced";
}

TEST_F(EditorViewportTest, ANewSceneIsANewPicture)
{
    ASSERT_FALSE(Create("Cube").empty());
    const Picture first = Frame0();
    ASSERT_TRUE(IsCube(first.Centre()));

    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Another"})).type, SceneInfoType);
    const Picture another = FrameSince(first.revision);
    EXPECT_TRUE(another.modified);
    EXPECT_TRUE(IsBlue(another.Centre()));
}

TEST_F(EditorViewportTest, ANewViewportSizeRendersAtThatSize)
{
    ASSERT_FALSE(Create("Cube").empty());
    const Picture first = Frame0();

    BufferWriter size;
    size.WriteI32(32);
    size.WriteI32(32);
    ASSERT_EQ(Execute(server, CommandType::SetViewportSize, size.Release()).type, OkType);
    const Picture square = FrameSince(first.revision);
    ASSERT_TRUE(square.modified);
    EXPECT_EQ(square.width, 32u);
    EXPECT_EQ(square.height, 32u);
    EXPECT_EQ(square.pixels.size(), 32u * 32u * 4u);
    EXPECT_TRUE(IsCube(square.Centre()));
    EXPECT_TRUE(IsBlue(square.At(1, 1)));

    // The projection follows the shape: with a square viewport, the same scale across and up (elements 0 and 5)
    const Frame reply = Execute(server, CommandType::GetEditorCamera);
    ASSERT_EQ(reply.type, EditorCameraType) << reply.Text();
    BufferReader r(reply.payload);
    // position, rotation, fovY, orthographic, orthoSize, nearPlane, farPlane, then the view matrix
    (void)r.ReadBytes(12 + 16 + 4 + 1 + 4 + 4 + 4 + 64);
    float projection[16];
    for (float &value : projection)
        value = r.ReadF32();
    EXPECT_NEAR(projection[0], projection[5], 1e-4f);
}

TEST_F(EditorViewportTest, FrameChangedFollowsTheScene)
{
    const Picture first = Frame0();
    ASSERT_TRUE(first.modified);

    const auto frameEvents = [this](const uint32_t after)
    {
        size_t count = 0;
        for (const json &event : server.GetEvents().Read(after, 4096, 0).events)
        {
            if (event.value("kind", "") == "frameChanged")
                ++count;
        }
        return count;
    };

    const uint32_t before = server.GetEvents().LastSeq();
    ASSERT_FALSE(Create("Cube").empty());
    ASSERT_FALSE(Create("Sphere").empty());
    ASSERT_FALSE(Create("Quad").empty());
    EXPECT_EQ(frameEvents(before), 1u) << "one for the three changes before a frame";

    ASSERT_TRUE(FrameSince(first.revision).modified);
    const uint32_t afterFrame = server.GetEvents().LastSeq();
    ASSERT_FALSE(Create("Cube").empty());
    EXPECT_EQ(frameEvents(afterFrame), 1u) << "and one more after the frame";
}

TEST_F(EditorViewportTest, ARefusedCameraLeavesTheFrameAlone)
{
    ASSERT_FALSE(Create("Cube").empty());
    const Picture first = Frame0();

    BufferWriter bad;
    for (const float v : {0.0f, 0.0f, 3.0f})
        bad.WriteF32(v);
    for (const float v : {0.0f, 0.0f, 0.0f, 0.0f}) // a zero quaternion
        bad.WriteF32(v);
    bad.WriteF32(60.0f);
    bad.WriteBool(false);
    bad.WriteF32(5.0f);
    bad.WriteF32(0.1f);
    bad.WriteF32(100.0f);
    EXPECT_EQ(Execute(server, CommandType::SetEditorCamera, bad.Release()).type, ErrorType);
    EXPECT_FALSE(FrameSince(first.revision).modified);
}

// ==================== The frame revision and its event ====================

TEST_F(EditorViewportTest, AReportedChangeMovesTheRevisionOnceAndTheEventNamesTheNextFramesRevision)
{
    const Picture first = Frame0();
    const uint32_t seq = server.GetEvents().LastSeq();

    ASSERT_FALSE(Create("Cube").empty());
    std::vector<uint32_t> announced;
    for (const json &event : server.GetEvents().Read(seq, 4096, 0).events)
    {
        if (event.value("kind", "") == "frameChanged")
            announced.push_back(event.at("revision").get<uint32_t>());
    }
    ASSERT_EQ(announced.size(), 1u);
    EXPECT_EQ(announced[0], first.revision + 1) << "one revision on, not two";

    const Picture next = FrameSince(first.revision);
    ASSERT_TRUE(next.modified);
    EXPECT_EQ(next.revision, announced[0]) << "the event announces the revision of the frame that follows";

    // Opening another scene is one change too
    const Picture beforeNew = next;
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Next"})).type, SceneInfoType);
    EXPECT_EQ(FrameSince(beforeNew.revision).revision, beforeNew.revision + 1);
}

TEST_F(EditorViewportTest, ATransformNudgeBelowTheMathLibrarysEpsilonLeavesSceneAndFrameInStep)
{
    const std::string cube = Create("Cube");
    ASSERT_FALSE(cube.empty());
    const Picture first = Frame0();
    const uint32_t sceneRevision = server.GetSceneRevision();

    BufferWriter w;
    w.WriteString(cube);
    for (const float v : {1.0e-7f, 0.0f, 0.0f}) // position
        w.WriteF32(v);
    for (const float v : {0.0f, 0.0f, 0.0f}) // Euler angles
        w.WriteF32(v);
    for (const float v : {1.0f, 1.0f, 1.0f}) // scale
        w.WriteF32(v);
    ASSERT_EQ(Execute(server, CommandType::SetEntityTransform, w.Release()).type, OkType);

    // The engine's own setters ignore a move this small (Positionable compares with an epsilon), so nothing changed:
    // the scene revision and the frame revision agree, whichever way that goes. What mustn't happen is the scene
    // changing without either moving.
    const bool sceneChanged = server.GetSceneRevision() != sceneRevision;
    EXPECT_EQ(FrameSince(first.revision).modified, sceneChanged);
}

// ==================== The colour space ====================

/// A project too, for SetProjectSettings
class EditorViewportProjectTest : public EditorViewportTest
{
protected:
    void SetUp() override
    {
        EditorViewportTest::SetUp();
        const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
        _base = std::filesystem::temp_directory_path() / "n2-editor-viewport-test" / info->name();
        std::error_code error;
        std::filesystem::remove_all(_base, error);
        const auto created = IO::CreateProject(_base / "Game");
        ASSERT_TRUE(created) << created.error().message;
        const IO::ProjectFile project = *created;
        const std::filesystem::path root = std::filesystem::canonical(_base / "Game");
        IO::ResourceUUID::Initialize(project.projectId);
        IO::ResourceLoader::Instance().Initialize(root, project.UserDataPath(_base / "user"));
        server.SetProject(root, project);
    }

    void TearDown() override
    {
        Rendering::RenderSettings::SetColorSpace(Renderer::Common::ColorSpace::Gamma);
        EditorViewportTest::TearDown();
        std::error_code error;
        std::filesystem::remove_all(_base.parent_path(), error);
    }

    std::filesystem::path _base;
};

TEST_F(EditorViewportProjectTest, ChangingTheColourSpaceMakesANewFrameWithOtherPixels)
{
    // A lit mid-grey cube, put into the scene directly (a project setting is what changes next, not the scene)
    auto cube = GameObject::Create("Grey");
    auto *meshRenderer = cube->AddComponent<Rendering::MeshRenderer>();
    meshRenderer->SetMesh(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube));
    auto grey = Rendering::Material::Create();
    grey->SetBaseColor(Common::Color{0.5f, 0.5f, 0.5f, 1.0f});
    meshRenderer->SetMaterial(0, grey);
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    SceneManager::GetCurSceneRef().AddRootGameObject(cube);

    const Picture gamma = Frame0();
    ASSERT_EQ(gamma.type, FrameUpdateType);
    EXPECT_FALSE(IsBlue(gamma.Centre())) << "the cube is drawn";

    BufferWriter settings;
    WriteJson(settings, json{{"rendering", {{"colorSpace", "linear"}}}});
    ASSERT_EQ(Execute(server, CommandType::SetProjectSettings, settings.Release()).type,
              static_cast<uint8_t>(ResponseType::ProjectInfo));

    const Picture linear = FrameSince(gamma.revision);
    ASSERT_TRUE(linear.modified) << "the colour space is a change";
    EXPECT_NE(linear.revision, gamma.revision);
    EXPECT_NE(linear.pixels, gamma.pixels) << "and the picture is not the same";
}
