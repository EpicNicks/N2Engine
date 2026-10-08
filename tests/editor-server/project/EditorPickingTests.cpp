#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Window.hpp>
#include <engine/common/Color.hpp>
#include <engine/config/ApplicationOptions.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// PickEntity and GetEntityBounds (#80, E7b) through EditorServer::ExecuteCommand, on a scene made with NewScene (an
// edit-mode scene: nothing attaches, so there are no physics bodies) and a real headless software renderer, so the
// picks can be compared with the pixels of the frames the editor shows. Sizes and positions are chosen so every
// expectation is clear of an edge.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t FrameUpdateType = static_cast<uint8_t>(ResponseType::FrameUpdate);
    constexpr uint8_t PickResultType = static_cast<uint8_t>(ResponseType::PickResult);
    constexpr uint8_t BoundsType = static_cast<uint8_t>(ResponseType::Bounds);

    constexpr int ViewWidth = 64;
    constexpr int ViewHeight = 48;
    constexpr float Tolerance = 1e-3f;

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

    struct Pick
    {
        uint8_t type = 0xEE;
        std::string entityId;
        std::array<float, 3> point{};
        float distance = 0.0f;
        std::string error;

        [[nodiscard]] bool Hit() const { return !entityId.empty(); }
    };

    struct CameraArgs
    {
        std::array<float, 3> position{0.0f, 0.0f, 3.0f};
        float fovY = 60.0f;
        bool orthographic = false;
        float orthoSize = 5.0f;
    };

    struct Rgb
    {
        int r = 0;
        int g = 0;
        int b = 0;
    };

    bool IsCube(const Rgb &c) { return c.r >= 200 && c.g >= 200 && c.b >= 200; }

    class EditorPickingTest : public ::testing::Test
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

            SetViewport(ViewWidth, ViewHeight);
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Picking Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            SetCamera(CameraArgs{});
        }

        void TearDown() override
        {
            // The scene's shapes release their GPU resources while the renderer is still alive
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            Application::GetInstance().GetWindow().Shutdown();
        }

        void SetViewport(const int width, const int height)
        {
            BufferWriter w;
            w.WriteI32(width);
            w.WriteI32(height);
            ASSERT_EQ(Execute(server, CommandType::SetViewportSize, w.Release()).type, OkType);
        }

        void SetCamera(const CameraArgs &camera)
        {
            BufferWriter w;
            for (const float v : camera.position)
                w.WriteF32(v);
            for (const float v : {0.0f, 0.0f, 0.0f, 1.0f})
                w.WriteF32(v);
            w.WriteF32(camera.fovY);
            w.WriteBool(camera.orthographic);
            w.WriteF32(camera.orthoSize);
            w.WriteF32(0.1f);
            w.WriteF32(100.0f);
            const Frame response = Execute(server, CommandType::SetEditorCamera, w.Release());
            ASSERT_EQ(response.type, OkType) << response.Text();
        }

        std::string Create(const std::string &preset, const std::string &parent = "")
        {
            BufferWriter w;
            w.WriteString("");
            w.WriteString(parent);
            w.WriteI32(-1);
            w.WriteString(preset);
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        void SetLocal(const std::string &id, const std::array<float, 3> &position,
                      const std::array<float, 3> &scale = {1.0f, 1.0f, 1.0f})
        {
            BufferWriter w;
            w.WriteString(id);
            for (const float v : position)
                w.WriteF32(v);
            for (const float v : {0.0f, 0.0f, 0.0f, 1.0f})
                w.WriteF32(v);
            for (const float v : scale)
                w.WriteF32(v);
            const Frame response = Execute(server, CommandType::SetLocalTransform, w.Release());
            ASSERT_EQ(response.type, OkType) << response.Text();
        }

        void SetActive(const std::string &id, const bool active)
        {
            BufferWriter w;
            w.WriteString(id);
            w.WriteString(json{{"active", active}}.dump());
            const Frame response = Execute(server, CommandType::SetEntityProperties, w.Release());
            ASSERT_NE(response.type, ErrorType) << response.Text();
        }

        Pick PickAt(const float x, const float y, const bool includeInactive = false)
        {
            BufferWriter w;
            w.WriteF32(x);
            w.WriteF32(y);
            w.WriteBool(includeInactive);
            const Frame response = Execute(server, CommandType::PickEntity, w.Release());
            Pick pick;
            pick.type = response.type;
            if (response.type != PickResultType)
            {
                pick.error = response.Text();
                return pick;
            }
            BufferReader r(response.payload);
            pick.entityId = r.ReadString();
            for (float &v : pick.point)
                v = r.ReadF32();
            pick.distance = r.ReadF32();
            return pick;
        }

        /// The pick at the middle of the viewport
        Pick PickMiddle(const bool includeInactive = false)
        {
            return PickAt(static_cast<float>(ViewWidth) / 2.0f, static_cast<float>(ViewHeight) / 2.0f, includeInactive);
        }

        Frame Bounds(const std::string &idsJson)
        {
            return Execute(server, CommandType::GetEntityBounds, Strings({idsJson}));
        }

        /// The answer to GetEntityBounds for these ids, as JSON (ADD_FAILURE and null for an Error)
        json BoundsOf(const std::vector<std::string> &ids)
        {
            const Frame response = Bounds(json(ids).dump());
            if (response.type != BoundsType)
            {
                ADD_FAILURE() << "not a Bounds response: " << response.Text();
                return json::array();
            }
            BufferReader r(response.payload);
            return json::parse(r.ReadString());
        }

        /// The pixels of the editor view's frame (RGBA, top row first)
        std::vector<uint8_t> FramePixels()
        {
            BufferWriter w;
            w.WriteU32(0);
            const Frame response = Execute(server, CommandType::RenderFrameIfChanged, w.Release());
            EXPECT_EQ(response.type, FrameUpdateType) << response.Text();
            BufferReader r(response.payload);
            (void)r.ReadU32();
            (void)r.ReadBool();
            (void)r.ReadU32();
            (void)r.ReadU32();
            const auto pixels = r.ReadBytes(r.Remaining());
            return {pixels.begin(), pixels.end()};
        }

        EditorServer server;
    };

    void ExpectPoint(const std::array<float, 3> &actual, const std::array<float, 3> &expected, const char *what)
    {
        for (std::size_t i = 0; i < 3; ++i)
        {
            EXPECT_NEAR(actual[i], expected[i], Tolerance) << what << " [" << i << "]";
        }
    }

    void ExpectBoxJson(const json &entry, const std::string &id, const std::array<float, 3> &min,
                       const std::array<float, 3> &max)
    {
        ASSERT_TRUE(entry.is_object());
        EXPECT_EQ(entry.at("id").get<std::string>(), id);
        const char *axes[] = {"x", "y", "z"};
        for (std::size_t i = 0; i < 3; ++i)
        {
            EXPECT_NEAR(entry.at("min").at(axes[i]).get<float>(), min[i], Tolerance) << "min " << axes[i];
            EXPECT_NEAR(entry.at("max").at(axes[i]).get<float>(), max[i], Tolerance) << "max " << axes[i];
        }
    }
}

// ==================== PickEntity ====================

TEST_F(EditorPickingTest, ACubeIsHitAtTheMiddleOfTheViewAndMissedAtTheCorners)
{
    const std::string cube = Create("Cube");
    ASSERT_FALSE(cube.empty());

    const Pick hit = PickMiddle();
    ASSERT_EQ(hit.type, PickResultType) << hit.error;
    EXPECT_EQ(hit.entityId, cube);
    // The camera is at z = 3 with its near plane at 0.1: the ray starts at z = 2.9 and meets the face at z = 0.5
    ExpectPoint(hit.point, {0.0f, 0.0f, 0.5f}, "point");
    EXPECT_NEAR(hit.distance, 2.4f, Tolerance);

    for (const auto &[x, y] : {std::pair{2.0f, 2.0f}, std::pair{61.0f, 2.0f}, std::pair{2.0f, 45.0f},
                               std::pair{61.0f, 45.0f}})
    {
        const Pick miss = PickAt(x, y);
        ASSERT_EQ(miss.type, PickResultType);
        EXPECT_FALSE(miss.Hit()) << x << "," << y;
        EXPECT_NEAR(miss.distance, 0.0f, Tolerance);
        ExpectPoint(miss.point, {0.0f, 0.0f, 0.0f}, "a miss's point");
    }
}

TEST_F(EditorPickingTest, TheOriginIsTheTopLeftAndYPointsDown)
{
    const std::string high = Create("Cube");
    SetLocal(high, {0.0f, 1.0f, 0.0f});

    // At z = 0 the view is 3.46 high, so the cube's middle (y = 1) is at 0.58 of the half height above the centre:
    // pixel row 24 - 0.58 * 24 = 10. The mirrored row is empty.
    EXPECT_EQ(PickAt(32.0f, 10.0f).entityId, high);
    EXPECT_FALSE(PickAt(32.0f, 38.0f).Hit()) << "the same distance below the middle";

    const std::string left = Create("Cube");
    SetLocal(left, {-1.0f, -1.0f, 0.0f});
    // Left of the middle (small x) and below it (large y)
    EXPECT_EQ(PickAt(15.0f, 38.0f).entityId, left);
    EXPECT_FALSE(PickAt(49.0f, 38.0f).Hit()) << "the same distance to the right";
}

TEST_F(EditorPickingTest, AnEmptyObjectIsPickedByItsSphere)
{
    const std::string empty = Create("Empty");
    ASSERT_FALSE(empty.empty());

    const Pick hit = PickMiddle();
    EXPECT_EQ(hit.entityId, empty);
    // The sphere's radius is 0.25 world units: the ray from z = 2.9 meets its front at z = 0.25
    ExpectPoint(hit.point, {0.0f, 0.0f, 0.25f}, "point");
    EXPECT_NEAR(hit.distance, 2.65f, Tolerance);

    // A light is picked the same way
    const std::string light = Create("PointLight");
    SetLocal(light, {0.0f, 1.0f, 0.0f});
    EXPECT_EQ(PickAt(32.0f, 10.0f).entityId, light);
}

TEST_F(EditorPickingTest, TheNearestObjectWins)
{
    const std::string back = Create("Cube");
    SetLocal(back, {0.0f, 0.0f, -2.0f});
    EXPECT_EQ(PickMiddle().entityId, back);

    const std::string front = Create("Cube");
    SetLocal(front, {0.0f, 0.0f, 1.0f});
    const Pick hit = PickMiddle();
    EXPECT_EQ(hit.entityId, front);
    ExpectPoint(hit.point, {0.0f, 0.0f, 1.5f}, "point");
}

TEST_F(EditorPickingTest, InactiveObjectsArePickedOnlyWhenAsked)
{
    const std::string cube = Create("Cube");
    SetActive(cube, false);
    EXPECT_FALSE(PickMiddle().Hit());
    EXPECT_EQ(PickMiddle(true).entityId, cube);

    // Under an inactive parent: inactive in the hierarchy
    SetActive(cube, true);
    const std::string group = Create("Empty");
    const std::string child = Create("Cube", group);
    SetLocal(group, {0.0f, 10.0f, 0.0f});
    SetActive(group, false);
    SetLocal(child, {0.0f, 0.0f, 0.0f});
    EXPECT_EQ(PickMiddle().entityId, cube) << "the inactive group's child is out of view as well";
    SetActive(cube, false);
    EXPECT_FALSE(PickMiddle().Hit());
}

TEST_F(EditorPickingTest, ChildrenFollowTheirParentsTransform)
{
    const std::string group = Create("Empty");
    SetLocal(group, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    const std::string child = Create("Cube", group);
    SetLocal(child, {0.0f, 0.0f, 0.0f});

    // The child sits at the group's position, up at pixel row 10
    EXPECT_EQ(PickAt(32.0f, 10.0f).entityId, child);
    EXPECT_FALSE(PickMiddle().Hit()) << "the middle of the view is below it";

    // The group's own pivot sphere is covered by its child here, which is nearer
    SetLocal(group, {0.0f, 1.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
    EXPECT_EQ(PickAt(32.0f, 10.0f).entityId, child);
}

TEST_F(EditorPickingTest, TheEditorCameraDecidesWhereTheRayGoes)
{
    const std::string cube = Create("Cube");
    SetLocal(cube, {50.0f, 0.0f, 0.0f});
    EXPECT_FALSE(PickMiddle().Hit());

    CameraArgs aside;
    aside.position = {50.0f, 0.0f, 3.0f};
    SetCamera(aside);
    EXPECT_EQ(PickMiddle().entityId, cube);

    // Orthographic with half height 1: the cube covers the middle half of the view
    CameraArgs ortho;
    ortho.position = {50.0f, 0.0f, 3.0f};
    ortho.orthographic = true;
    ortho.orthoSize = 1.0f;
    SetCamera(ortho);
    EXPECT_EQ(PickAt(24.0f, 16.0f).entityId, cube);
    EXPECT_EQ(PickAt(40.0f, 32.0f).entityId, cube);
    EXPECT_FALSE(PickAt(10.0f, 24.0f).Hit());
    EXPECT_FALSE(PickAt(32.0f, 6.0f).Hit());
}

TEST_F(EditorPickingTest, ThePointIsInViewportPixelsWhateverItsSize)
{
    const std::string cube = Create("Cube");
    // The 4 times larger viewport: the same view, so the same cube covers the same share of it
    SetViewport(256, 192);
    EXPECT_EQ(PickAt(128.0f, 96.0f).entityId, cube);
    EXPECT_EQ(PickAt(110.0f, 80.0f).entityId, cube);
    EXPECT_FALSE(PickAt(10.0f, 10.0f).Hit());
    EXPECT_FALSE(PickAt(250.0f, 96.0f).Hit());
}

TEST_F(EditorPickingTest, APointOutsideTheViewportPicksNothing)
{
    ASSERT_FALSE(Create("Cube").empty());
    EXPECT_TRUE(PickAt(32.0f, 24.0f).Hit());
    for (const auto &[x, y] : {std::pair{-1.0f, 24.0f}, std::pair{32.0f, -1.0f}, std::pair{64.0f, 24.0f},
                               std::pair{32.0f, 48.0f}, std::pair{1000.0f, 1000.0f}})
    {
        const Pick pick = PickAt(x, y);
        EXPECT_EQ(pick.type, PickResultType) << x << "," << y;
        EXPECT_FALSE(pick.Hit()) << x << "," << y;
    }
}

TEST_F(EditorPickingTest, ANonFinitePointIsRefused)
{
    ASSERT_FALSE(Create("Cube").empty());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const auto &[x, y] : {std::pair{nan, 24.0f}, std::pair{32.0f, nan}, std::pair{inf, 24.0f},
                               std::pair{32.0f, -inf}})
    {
        const Pick pick = PickAt(x, y);
        EXPECT_EQ(pick.type, ErrorType);
        EXPECT_FALSE(pick.error.empty());
    }
}

TEST_F(EditorPickingTest, AnEmptySceneAndAnObjectWithoutATransformPickNothing)
{
    EXPECT_FALSE(PickMiddle().Hit());

    // CreateEntity (the old command) makes an object with no transform: no place in the world
    const Frame made = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(made.type, EntityCreatedType) << made.Text();
    EXPECT_FALSE(PickMiddle().Hit());
    EXPECT_FALSE(PickMiddle(true).Hit());
}

TEST_F(EditorPickingTest, ItWorksOnASceneThatWasNeverAttached)
{
    // The host's scenes are in edit mode: components never attach, so a physics pick could find nothing here
    Scene *scene = SceneManager::GetCurScene();
    ASSERT_NE(scene, nullptr);
    EXPECT_TRUE(scene->IsEditMode());
    const std::string cube = Create("Cube");
    EXPECT_EQ(PickMiddle().entityId, cube);
}

TEST_F(EditorPickingTest, ItAgreesWithTheRenderedFrame)
{
    const std::string cube = Create("Cube");
    ASSERT_FALSE(cube.empty());
    const std::vector<uint8_t> pixels = FramePixels();
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(ViewWidth) * ViewHeight * 4);

    // The cube's front face covers about +-8.3 pixels either way around the middle (see the geometry: half a unit
    // at 2.5 away, 60 degrees, 64 x 48). Pixels within 2 of an edge are left out; every other one must agree.
    constexpr float half = 8.3f;
    int checked = 0;
    for (int y = 1; y < ViewHeight; y += 3)
    {
        for (int x = 1; x < ViewWidth; x += 3)
        {
            const float dx = std::fabs(static_cast<float>(x) + 0.5f - ViewWidth / 2.0f);
            const float dy = std::fabs(static_cast<float>(y) + 0.5f - ViewHeight / 2.0f);
            if (std::fabs(dx - half) < 2.0f || std::fabs(dy - half) < 2.0f)
                continue;
            const std::size_t i = (static_cast<std::size_t>(y) * ViewWidth + static_cast<std::size_t>(x)) * 4;
            const bool drawn = IsCube(Rgb{pixels[i], pixels[i + 1], pixels[i + 2]});
            const Pick pick = PickAt(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            EXPECT_EQ(pick.Hit(), drawn) << "pixel " << x << "," << y;
            EXPECT_EQ(drawn, dx < half && dy < half) << "pixel " << x << "," << y << " (the geometry)";
            ++checked;
        }
    }
    EXPECT_GT(checked, 100);
}

TEST_F(EditorPickingTest, PickingChangesNothingAndLogsNothing)
{
    ASSERT_FALSE(Create("Cube").empty());
    BufferWriter since;
    since.WriteU32(0);
    const Frame first = Execute(server, CommandType::RenderFrameIfChanged, since.Release());
    ASSERT_EQ(first.type, FrameUpdateType);
    const uint32_t revision = server.GetFrameRevision();
    const uint32_t rendered = server.GetEditorFramesRendered();
    const uint32_t seq = server.GetEvents().LastSeq();

    for (int i = 0; i < 50; ++i)
    {
        (void)PickAt(32.0f, 24.0f);
        (void)PickAt(2.0f, 2.0f);
        (void)Bounds("[]");
    }
    // A client sends these on every click or mouse move: they move no revision and add no event (a refusal does log,
    // as any failed command does, but none is made here)
    EXPECT_EQ(server.GetFrameRevision(), revision);
    EXPECT_EQ(server.GetEditorFramesRendered(), rendered);
    EXPECT_EQ(server.GetEvents().LastSeq(), seq);
}

// ==================== GetEntityBounds ====================

TEST_F(EditorPickingTest, TheBoundsOfACubeAreItsWorldBox)
{
    const std::string cube = Create("Cube");
    SetLocal(cube, {1.0f, 2.0f, 3.0f}, {2.0f, 4.0f, 6.0f});

    const json bounds = BoundsOf({cube});
    ASSERT_EQ(bounds.size(), 1u);
    ExpectBoxJson(bounds[0], cube, {0.0f, 0.0f, 0.0f}, {2.0f, 4.0f, 6.0f});
}

TEST_F(EditorPickingTest, TheBoundsOfAHierarchyFollowItsTransforms)
{
    const std::string group = Create("Empty");
    SetLocal(group, {10.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
    const std::string a = Create("Cube", group);
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    const std::string b = Create("Cube", group);
    SetLocal(b, {-1.0f, 3.0f, 0.0f});

    // a is at (12, 0, 0) with scale 2: x 11 to 13, y and z -1 to 1. b is at (8, 6, 0): x 7 to 9, y 5 to 7.
    const json bounds = BoundsOf({group, a, b});
    ASSERT_EQ(bounds.size(), 3u);
    ExpectBoxJson(bounds[0], group, {7.0f, -1.0f, -1.0f}, {13.0f, 7.0f, 1.0f});
    ExpectBoxJson(bounds[1], a, {11.0f, -1.0f, -1.0f}, {13.0f, 1.0f, 1.0f});
    ExpectBoxJson(bounds[2], b, {7.0f, 5.0f, -1.0f}, {9.0f, 7.0f, 1.0f});
}

TEST_F(EditorPickingTest, AnObjectWithNothingToMeasureGetsItsPickSphereBox)
{
    const std::string empty = Create("Empty");
    SetLocal(empty, {1.0f, 2.0f, 3.0f});
    const json bounds = BoundsOf({empty});
    ASSERT_EQ(bounds.size(), 1u);
    ExpectBoxJson(bounds[0], empty, {0.75f, 1.75f, 2.75f}, {1.25f, 2.25f, 3.25f});
}

TEST_F(EditorPickingTest, InactiveChildrenDontCountButAnInactiveObjectAskedAboutDoes)
{
    const std::string group = Create("Empty");
    const std::string shown = Create("Cube", group);
    const std::string hidden = Create("Cube", group);
    SetLocal(hidden, {50.0f, 0.0f, 0.0f});
    SetActive(hidden, false);

    json bounds = BoundsOf({group, hidden});
    ASSERT_EQ(bounds.size(), 2u);
    ExpectBoxJson(bounds[0], group, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
    ExpectBoxJson(bounds[1], hidden, {49.5f, -0.5f, -0.5f}, {50.5f, 0.5f, 0.5f});
    (void)shown;
}

TEST_F(EditorPickingTest, UnknownIdsAndObjectsWithoutAPlaceAreLeftOut)
{
    const std::string cube = Create("Cube");
    const Frame made = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(made.type, EntityCreatedType);
    BufferReader r(made.payload);
    const std::string bare = r.ReadString();

    const json bounds = BoundsOf({"not-an-id", "00000000-0000-0000-0000-000000000000", bare, cube, cube});
    ASSERT_EQ(bounds.size(), 1u) << bounds.dump() << ": the stale ids and the bare object are left out, and a repeat is once";
    EXPECT_EQ(bounds[0].at("id").get<std::string>(), cube);
    EXPECT_EQ(BoundsOf({}).size(), 0u);
}

TEST_F(EditorPickingTest, TheBoundsArrayIsCheckedAndBounded)
{
    EXPECT_EQ(Bounds("{}").type, ErrorType);
    EXPECT_EQ(Bounds("\"id\"").type, ErrorType);
    EXPECT_EQ(Bounds("[1, 2]").type, ErrorType) << "ids are strings";
    EXPECT_EQ(Bounds("[null]").type, ErrorType);
    EXPECT_EQ(Bounds("not json").type, ErrorType);

    json many = json::array();
    for (std::size_t i = 0; i < EditorServer::MaxBoundsEntityIds; ++i)
        many.push_back("x");
    EXPECT_EQ(Bounds(many.dump()).type, BoundsType) << "the most that is answered";
    many.push_back("x");
    EXPECT_EQ(Bounds(many.dump()).type, ErrorType);
}

TEST_F(EditorPickingTest, TheBoundsOfWhatWasPickedContainThePickedPoint)
{
    const std::string cube = Create("Cube");
    SetLocal(cube, {0.0f, 1.0f, 0.0f}, {1.5f, 1.0f, 1.0f});
    const Pick hit = PickAt(32.0f, 10.0f);
    ASSERT_EQ(hit.entityId, cube);

    const json bounds = BoundsOf({hit.entityId});
    ASSERT_EQ(bounds.size(), 1u);
    const char *axes[] = {"x", "y", "z"};
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_GE(hit.point[i] + Tolerance, bounds[0].at("min").at(axes[i]).get<float>()) << axes[i];
        EXPECT_LE(hit.point[i] - Tolerance, bounds[0].at("max").at(axes[i]).get<float>()) << axes[i];
    }
}
