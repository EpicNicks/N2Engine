// Picking 2D colliders (issue #445): PointerDispatcher::PickWorld and the OnMouse* callbacks through Physics2D::PickRay, with
// orthographic and perspective cameras, each collider in the plane of its object's z; the pick mask, triggers, inactive objects;
// and, with PhysX, the nearer of a 3D and a 2D hit along the same ray. docs/input.html#picking-2d

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DQueries.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#ifdef N2ENGINE_PHYSX_ENABLED
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#endif

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace N2Engine::Physics2D;
using Math::Vector2;
using Math::Vector3;

namespace
{
    const Vector2i Viewport{800, 600};

    class Clicks2D final : public Component
    {
    public:
        explicit Clicks2D(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Clicks2D"; }

        void OnMouseEnter() override { ++enter; }
        void OnMouseExit() override { ++exits; }
        void OnMouseDown() override { ++down; }
        void OnMouseUpAsButton() override { ++clicks; }

        int enter = 0, exits = 0, down = 0, clicks = 0;
    };

    class Picking2DTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;
        Camera _camera;
        PointerDispatcher _dispatcher;
        Mouse _mouse{nullptr};

        void SetUp() override
        {
            Layers::ResetToDefaults();
            SceneManager::AddScene(Scene::Create(std::string("Picking2D_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
            // A 2D camera: 8 x 6 units in an 800 x 600 window (100 pixels a unit), looking down -z from z = 10
            _camera.SetOrthographic(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
            _camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
            _dispatcher.SetWorldHitProvider([this](const Vector2 &screen)
                                            { return PointerDispatcher::PickWorld(_camera, screen, Viewport, _dispatcher.GetPickMask()); });
        }

        void TearDown() override
        {
            SceneManager::AddScene(Scene::Create("Picking2D_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Layers::ResetToDefaults();
        }

        GameObject::Ptr Spawn(const std::string &name, const Vector3 &position, const int layer = Layers::Default)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(position);
            go->SetLayer(layer);
            go->AddComponent<Clicks2D>();
            _scene->AddRootGameObject(go);
            return go;
        }

        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const int layer = Layers::Default)
        {
            auto go = Spawn(name, position, layer);
            go->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.5f, 0.5f));
            return go;
        }

        /// The window point the orthographic camera shows world (x, y) at
        static Vector2 ScreenOf(const float x, const float y) { return Vector2(400.0f + x * 100.0f, 300.0f - y * 100.0f); }

        [[nodiscard]] GameObject *Pick(const Vector2 &screen) const
        {
            return PointerDispatcher::PickWorld(_camera, screen, Viewport, _dispatcher.GetPickMask());
        }

        void Frame(const Vector2 &screen, const bool held)
        {
            _mouse.InjectPointer(screen, held ? Mouse::ButtonBit(0) : 0u);
            _mouse.Update();
            _dispatcher.Process(PointerState::FromMouse(_mouse));
        }
    };
}

#ifdef N2ENGINE_BOX2D_ENABLED

TEST_F(Picking2DTest, OrthographicClickSendsOnMouseCallbacksToA2DCollider)
{
    const auto box = SpawnBox("Box", Vector3(2.0f, 1.0f, 0.0f));
    _scene->ProcessAttachQueue(); // pickable before any fixed step, as in 3D
    auto *clicks = box->GetComponent<Clicks2D>();

    Frame(ScreenOf(2.2f, 0.7f), true);
    EXPECT_EQ(clicks->enter, 1);
    EXPECT_EQ(clicks->down, 1);
    EXPECT_EQ(_dispatcher.GetCaptured(), box.get());
    Frame(ScreenOf(2.2f, 0.7f), false);
    EXPECT_EQ(clicks->clicks, 1);

    // Off the box: an exit, and nothing is hovered
    Frame(ScreenOf(0.0f, 0.0f), false);
    EXPECT_EQ(clicks->exits, 1);
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
}

TEST_F(Picking2DTest, EveryShapeAndBodyTypeIsPickable)
{
    const auto circle = Spawn("Circle", Vector3(-2.0f, 0.0f, 0.0f));
    circle->AddComponent<CircleCollider2D>()->SetRadius(0.5f);
    const auto polygon = Spawn("Polygon", Vector3(0.0f, 0.0f, 0.0f));
    polygon->AddComponent<PolygonCollider2D>()->SetVertices({{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}});
    const auto dynamic = Spawn("Dynamic", Vector3(2.0f, 0.0f, 0.0f));
    dynamic->AddComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    dynamic->AddComponent<BoxCollider2D>();
    _scene->ProcessAttachQueue();

    EXPECT_EQ(Pick(ScreenOf(-2.0f, 0.4f)), circle.get());
    EXPECT_EQ(Pick(ScreenOf(-1.6f, 0.4f)), nullptr) << "outside the circle, inside its bounding box";
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.3f)), polygon.get());
    EXPECT_EQ(Pick(ScreenOf(0.4f, 0.4f)), nullptr) << "outside the triangle";
    EXPECT_EQ(Pick(ScreenOf(2.3f, -0.3f)), dynamic.get());
}

TEST_F(Picking2DTest, TheColliderNearestTheCameraWins)
{
    // The same x and y; the camera is at z = 10 looking down -z, so the one with the higher z is in front
    const auto back = SpawnBox("Back", Vector3(0.0f, 0.0f, -1.0f));
    const auto front = SpawnBox("Front", Vector3(0.0f, 0.0f, 2.0f));
    _scene->ProcessAttachQueue();
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), front.get());

    front->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, -3.0f));
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), back.get()) << "the plane follows the object's z at once";

    // Behind the camera, or past the far plane: not seen, so not picked
    back->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, 20.0f));
    front->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, -200.0f));
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), nullptr);
}

TEST_F(Picking2DTest, PerspectiveCameraPicksEachColliderInItsOwnPlane)
{
    _camera.SetPerspective(60.0f, 800.0f / 600.0f, 0.1f, 100.0f);
    const Vector2 screen(650.0f, 180.0f);
    const Math::Ray ray = _camera.ScreenPointToRay(screen, Viewport);
    const auto crossing = [&ray](const float z)
    {
        const float distance = (z - ray.origin.z) / ray.direction.z;
        return Vector3(ray.origin.x + ray.direction.x * distance, ray.origin.y + ray.direction.y * distance, z);
    };

    // Where the ray crosses z = -5, and an object at the same x and y but z = 0, which the ray crosses elsewhere
    const Vector3 deep = crossing(-5.0f);
    const auto farCircle = Spawn("Far", deep);
    farCircle->AddComponent<CircleCollider2D>()->SetRadius(0.3f);
    const auto nearCircle = Spawn("Near", Vector3(deep.x, deep.y, 0.0f));
    nearCircle->AddComponent<CircleCollider2D>()->SetRadius(0.3f);
    _scene->ProcessAttachQueue();

    ASSERT_GT((Vector2(crossing(0.0f).x, crossing(0.0f).y) - Vector2(deep.x, deep.y)).Length(), 1.0f) << "the test needs a slanted ray";
    EXPECT_EQ(Pick(screen), farCircle.get());

    // Directly at the near one's crossing
    nearCircle->GetPositionable()->SetPosition(crossing(0.0f));
    _scene->ProcessAttachQueue();
    RaycastHit2D hit;
    ASSERT_TRUE(Physics2D::PickRay(ray.origin, ray.direction, hit)); // a static body follows its transform in the next fixed step
    EXPECT_EQ(hit.gameObject, farCircle.get()) << "the 2D world still has the near circle where it was";
    ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
    EXPECT_EQ(Pick(screen), nearCircle.get());
    ASSERT_TRUE(Physics2D::PickRay(ray.origin, ray.direction, hit));
    EXPECT_NEAR(hit.distance, (0.0f - ray.origin.z) / ray.direction.z, 1e-3f) << "the distance is along the 3D ray";
}

TEST_F(Picking2DTest, PickMaskTriggersAndInactiveObjects)
{
    const auto shield = SpawnBox("Shield", Vector3(0.0f, 0.0f, 3.0f), Layers::IgnoreRaycast);
    const auto trigger = SpawnBox("Trigger", Vector3(0.0f, 0.0f, 2.0f));
    trigger->GetComponent<BoxCollider2D>()->SetIsTrigger(true);
    const auto target = SpawnBox("Target", Vector3(0.0f, 0.0f, 0.0f));
    _scene->ProcessAttachQueue();

    // Ignore Raycast is out of the default mask; a trigger is picked, as in 3D
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), trigger.get());
    _dispatcher.SetPickMask(Layers::AllLayers);
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), shield.get());
    _dispatcher.SetPickMask(Layers::DefaultRaycastMask);

    // An inactive object is never a target, and the pick goes through it
    trigger->SetActive(false);
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), target.get());
    target->SetActive(false);
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), nullptr);
}

TEST_F(Picking2DTest, PickRayRejectsRaysThatCannotCrossThePlane)
{
    SpawnBox("Box", Vector3(0.0f, 0.0f, 0.0f));
    _scene->ProcessAttachQueue();
    RaycastHit2D hit;
    EXPECT_TRUE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f), hit));
    EXPECT_FALSE(Physics2D::PickRay(Vector3(-5.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), hit)) << "parallel to the plane";
    EXPECT_FALSE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 1.0f), hit)) << "pointing away";
    EXPECT_FALSE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f), hit, 4.0f)) << "too short";
    EXPECT_FALSE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), hit)) << "no direction";
    EXPECT_FALSE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f), hit, kNoLimit, 0u)) << "empty mask";
}

#ifdef N2ENGINE_PHYSX_ENABLED

// With PhysX: the nearer of the 3D and the 2D hit along the same ray is the target
class Picking2DWith3DTest : public Picking2DTest
{
protected:
    static void SetUpTestSuite()
    {
        auto backend = std::make_unique<Physics::PhysXBackend>();
        ASSERT_TRUE(backend->Initialize());
        Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
    }

    static void TearDownTestSuite()
    {
        SceneManager::AddScene(Scene::Create("Picking2DWith3D_Teardown"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }
};

TEST_F(Picking2DWith3DTest, TheNearerOfA3DAndA2DHitWins)
{
    const auto cube = Spawn("Cube", Vector3(0.0f, 0.0f, 0.0f)); // its front face at z = 1
    cube->AddComponent<Physics::BoxCollider>()->SetSize(Vector3(2.0f, 2.0f, 2.0f));
    const auto sprite = SpawnBox("Sprite", Vector3(0.0f, 0.0f, -2.0f));
    _scene->ProcessAttachQueue();

    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), cube.get()) << "the 2D box is behind the cube";
    sprite->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, 3.0f));
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), sprite.get()) << "the 2D box is in front of the cube";

    Frame(ScreenOf(0.0f, 0.0f), true);
    EXPECT_EQ(sprite->GetComponent<Clicks2D>()->down, 1);
    EXPECT_EQ(cube->GetComponent<Clicks2D>()->down, 0);
}

#endif

#else

TEST_F(Picking2DTest, WithoutBox2DNo2DColliderIsPicked)
{
    SpawnBox("Box", Vector3(0.0f, 0.0f, 0.0f));
    _scene->ProcessAttachQueue();
    EXPECT_EQ(Pick(ScreenOf(0.0f, 0.0f)), nullptr);
    RaycastHit2D hit;
    EXPECT_FALSE(Physics2D::PickRay(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f), hit));
}

#endif
