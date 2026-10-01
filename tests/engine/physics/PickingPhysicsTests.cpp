#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Picking end to end in a real PhysX scene: an injected click goes through Camera::ScreenPointToRay and
// Raycast::Single to OnMouseDown on the box under it; and the Lua Physics.Raycast/RaycastAll bindings
#ifdef N2ENGINE_PHYSX_ENABLED

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace N2Engine::Physics;
using Math::Vector2;
using Math::Vector3;

namespace
{
    class ClickCounter final : public Component
    {
    public:
        explicit ClickCounter(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ClickCounter"; }

        void OnMouseEnter() override { ++enter; }
        void OnMouseExit() override { ++exits; }
        void OnMouseDown() override { ++down; }
        void OnMouseUpAsButton() override { ++clicks; }

        int enter = 0, exits = 0, down = 0, clicks = 0;
    };

    const Vector2i Viewport{800, 600};
    const Vector2 Centre(400.0f, 300.0f);
}

class PickingPhysicsTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
        auto backend = std::make_unique<PhysXBackend>();
        ASSERT_TRUE(backend->Initialize());
        Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
    }

    static void TearDownTestSuite()
    {
        SceneManager::AddScene(Scene::Create("PickingPhysicsTest_Teardown"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }

    Scene *_scene = nullptr;
    Camera _camera;
    PointerDispatcher _dispatcher;
    Mouse _mouse{nullptr};

    void SetUp() override
    {
        Layers::ResetToDefaults();
        SceneManager::AddScene(Scene::Create(std::string("PickingPhysics_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();

        // Looking down -z from z = 10, as the application's default camera does
        _camera.SetPerspective(60.0f, 800.0f / 600.0f, 0.1f, 100.0f);
        _camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
        _dispatcher.SetWorldHitProvider([this](const Vector2 &screen)
        {
            return PointerDispatcher::PickWorld(_camera, screen, Viewport, _dispatcher.GetPickMask());
        });
    }

    void TearDown() override { Layers::ResetToDefaults(); }

    static IPhysicsBackend &Backend() { return *Application::GetInstance().Get3DPhysicsBackend(); }

    void Step()
    {
        _scene->ProcessAttachQueue();
        Backend().ApplyPendingChanges();
        _scene->FixedUpdate();
        Backend().Update(1.0f / 60.0f);
        Backend().SyncTransforms();
        Backend().ProcessCollisionCallbacks();
        _scene->ProcessDestroyed();
    }

    GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const int layer = Layers::Default)
    {
        const auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        go->SetLayer(layer);
        go->AddComponent<BoxCollider>()->SetSize(Vector3(2.0f, 2.0f, 2.0f));
        go->AddComponent<ClickCounter>();
        _scene->AddRootGameObject(go);
        return go;
    }

    void Frame(const Vector2 &screen, const bool held)
    {
        _mouse.InjectPointer(screen, held ? Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(PointerState::FromMouse(_mouse));
    }

    static sol::state &Lua() { return Scripting::LuaRuntime::Instance().GetState(); }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result =
            Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }
};

TEST_F(PickingPhysicsTest, ClickOverABoxSendsItOnMouseDown)
{
    const auto box = SpawnBox("Clickable", Vector3(0.0f, 0.0f, 0.0f));
    Step();
    auto *counter = box->GetComponent<ClickCounter>();

    Frame(Centre, true);
    EXPECT_EQ(counter->down, 1);
    EXPECT_EQ(counter->enter, 1);
    EXPECT_EQ(_dispatcher.GetCaptured(), box.get());

    Frame(Centre, false);
    EXPECT_EQ(counter->clicks, 1);

    // The top-left corner of the view misses the box
    Frame(Vector2(0.0f, 0.0f), true);
    EXPECT_EQ(counter->down, 1);
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
}

TEST_F(PickingPhysicsTest, IgnoreRaycastLayerCannotBeClicked)
{
    const auto shield = SpawnBox("Shield", Vector3(0.0f, 0.0f, 4.0f), Layers::IgnoreRaycast);
    const auto target = SpawnBox("Target", Vector3(0.0f, 0.0f, -4.0f));
    Step();

    Frame(Centre, true);
    EXPECT_EQ(shield->GetComponent<ClickCounter>()->down, 0);
    EXPECT_EQ(target->GetComponent<ClickCounter>()->down, 1) << "the click went through the Ignore Raycast box";

    // With every layer in the pick mask the nearer box wins
    _dispatcher.SetPickMask(Layers::AllLayers);
    Frame(Centre, false);
    Frame(Centre, true);
    EXPECT_EQ(shield->GetComponent<ClickCounter>()->down, 1);
}

TEST_F(PickingPhysicsTest, PickableBeforeAnyFixedStep)
{
    // As while paused (timeScale 0): the collider is attached but no fixed step has run since
    const auto box = SpawnBox("Unstepped", Vector3(0.0f, 0.0f, 0.0f));
    _scene->ProcessAttachQueue();

    Frame(Centre, true);
    EXPECT_EQ(box->GetComponent<ClickCounter>()->down, 1);
}

TEST_F(PickingPhysicsTest, BeyondTheFarPlaneIsNotPicked)
{
    const auto distant = SpawnBox("Distant", Vector3(0.0f, 0.0f, -150.0f)); // the far plane is at z = -90
    Step();

    Frame(Centre, true);
    EXPECT_EQ(distant->GetComponent<ClickCounter>()->down, 0);
}

TEST_F(PickingPhysicsTest, CursorOutsideTheWindowPicksNothing)
{
    // Straddles the left edge of the view: a ray through x = 5 px hits it, and so would one through the
    // off-window x = -10 px (where GLFW reports a cursor left of the window on Windows), if it were cast
    const auto edge = SpawnBox("Edge", Vector3(-7.9f, 0.0f, 0.0f));
    Step();
    auto *counter = edge->GetComponent<ClickCounter>();

    const Vector2 offWindow(-10.0f, 300.0f);
    const Math::Ray offRay = _camera.ScreenPointToRay(offWindow, Viewport);
    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(offRay.origin, offRay.direction, hit, 100.0f)) << "the setup must put the box there";

    EXPECT_EQ(PointerDispatcher::PickWorld(_camera, offWindow, Viewport, Layers::DefaultRaycastMask), nullptr);
    EXPECT_EQ(PointerDispatcher::PickWorld(_camera, Vector2(400.0f, 600.0f), Viewport, Layers::AllLayers), nullptr)
        << "the bottom edge is outside too";

    Frame(Vector2(5.0f, 300.0f), false);
    ASSERT_EQ(counter->enter, 1);
    EXPECT_EQ(_dispatcher.GetHovered(), edge.get());

    // Leaving the window is leaving the object
    Frame(offWindow, false);
    EXPECT_EQ(counter->exits, 1);
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
}

TEST_F(PickingPhysicsTest, LuaRaycastReturnsHitTables)
{
    SpawnBox("LuaNear", Vector3(0.0f, 0.0f, 0.0f));
    SpawnBox("LuaFar", Vector3(0.0f, 0.0f, -6.0f), 9);
    Step();

    EXPECT_EQ(Eval<std::string>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 50).gameObject:GetName()"),
              "LuaNear");
    EXPECT_NEAR(Eval<float>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 50).distance"), 9.0f, 0.01f);
    EXPECT_NEAR(Eval<float>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1)).point.z"), 1.0f, 0.01f)
        << "the default distance is long enough";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), math.huge) ~= nil"))
        << "an infinite distance is clamped, not passed to PhysX";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 1e30) ~= nil"));
    EXPECT_NEAR(Eval<float>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 50).collider:GetSize().x"), 2.0f,
                0.01f) << "the collider comes back as its own type";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 50).rigidbody == nil"));

    EXPECT_EQ(Eval<std::string>(
                  "Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), nil, Layers.MaskOf(9)).gameObject:GetName()"),
              "LuaFar");
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), 5) == nil")) << "too short";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, 0), 50) == nil")) << "zero direction";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 10), Vector3(0, 0, -1), -1) == nil")) << "negative distance";

    EXPECT_EQ(Eval<int>("#Physics.RaycastAll(Vector3(0, 0, 10), Vector3(0, 0, -1), 50)"), 2);
    EXPECT_EQ(Eval<std::string>("Physics.RaycastAll(Vector3(0, 0, 10), Vector3(0, 0, -1), 50)[2].gameObject:GetName()"),
              "LuaFar") << "nearest first";
    EXPECT_EQ(Eval<int>("#Physics.RaycastAll(Vector3(0, 0, 10), Vector3(0, 0, -1), 50, 0)"), 0);
    EXPECT_EQ(Eval<int>("#Physics.RaycastAll(Vector3(0, 0, 10), Vector3(0, 0, -1), 50, -1)"), 2) << "-1 is every layer";
}

TEST_F(PickingPhysicsTest, LuaRayFromTheCameraHitsTheBox)
{
    SpawnBox("LuaPicked", Vector3(0.0f, 0.0f, 0.0f));
    Step();

    Lua()["picking_physics_camera"] = &_camera;
    EXPECT_EQ(Eval<std::string>(R"((function()
        local ray = picking_physics_camera:ScreenPointToRay(400, 300, 800, 600)
        return Physics.Raycast(ray.origin, ray.direction, 100).gameObject:GetName()
    end)())"), "LuaPicked");
    Lua()["picking_physics_camera"] = sol::lua_nil;
}

#endif
