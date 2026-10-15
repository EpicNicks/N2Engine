#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// Iteration order of the PhysX backend's re-filtering and touch maps: by body creation, not by address or hash.
// Only built on legs that have PhysX.
#ifdef N2ENGINE_PHYSX_ENABLED

using namespace N2Engine;
using namespace N2Engine::Physics;
using Math::Vector3;

namespace
{
    constexpr int LayerA = 8;

    std::vector<std::string> g_exits;

    // Appends the object's name to g_exits whenever it gets a collision Exit
    class ExitRecorder final : public Component
    {
    public:
        explicit ExitRecorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ExitRecorder"; }
        void OnCollisionExit(const Collision &) override { g_exits.push_back(GetGameObject().GetName()); }
    };
}

class PhysicsOrderTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto backend = std::make_unique<PhysXBackend>();
        ASSERT_TRUE(backend->Initialize());
        Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
    }

    static void TearDownTestSuite()
    {
        SceneManager::AddScene(Scene::Create("PhysicsOrderTest_Teardown"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        Layers::ResetToDefaults();
        SceneManager::AddScene(Scene::Create(std::string("PhysicsOrder_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
        g_exits.clear();
    }

    void TearDown() override
    {
        static_cast<PhysXBackend &>(Backend()).SetRefilterObserver({});
        Layers::ResetToDefaults();
    }

    static IPhysicsBackend &Backend() { return *Application::GetInstance().Get3DPhysicsBackend(); }

    void Step(const int steps = 1)
    {
        for (int i = 0; i < steps; ++i)
        {
            _scene->ProcessAttachQueue();
            Backend().ApplyPendingChanges();
            _scene->FixedUpdate();
            Backend().Update(1.0f / 60.0f);
            Backend().SyncTransforms();
            Backend().ProcessCollisionCallbacks();
            _scene->ProcessDestroyed();
        }
    }

    GameObject::Ptr Spawn(const std::string &name, const Vector3 &position, const int layer)
    {
        const auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        go->SetLayer(layer);
        _scene->AddRootGameObject(go);
        return go;
    }
};

// Layers::SetCollision re-filters every shape whose row changed. The bodies must be visited in the order they
// were created whatever order their colliders were allocated in or their positions suggest.
TEST_F(PhysicsOrderTest, CollisionMatrixRefilterVisitsBodiesInCreationOrder)
{
    constexpr int kBodies = 12;
    std::vector<GameObject::Ptr> objects;
    std::vector<std::string> created;
    for (int i = 0; i < kBodies; ++i)
    {
        // Positions deliberately not in creation order; far apart so nothing touches
        const float x = static_cast<float>((i * 7) % kBodies) * 10.0f;
        const std::string name = "Static" + std::to_string(i);
        objects.push_back(Spawn(name, Vector3(x, 0.0f, 0.0f), LayerA));
        objects.back()->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        created.push_back(name);
    }
    Step(2);

    std::vector<std::string> visited;
    static_cast<PhysXBackend &>(Backend()).SetRefilterObserver(
        [&visited](const GameObject &owner) { visited.push_back(owner.GetName()); });

    Layers::SetCollision(LayerA, LayerA, false); // changes the collision row of every shape on LayerA

    EXPECT_EQ(visited, created);
}

// A body's destruction ends its touches; the Exit events go out in the touch map's order, which is by body
// creation (the floor is the oldest body, so the pairs sort by the other body).
TEST_F(PhysicsOrderTest, ForgottenTouchesEndInCreationOrder)
{
    const auto floor = Spawn("Floor", Vector3(0.0f, 0.0f, 0.0f), 0);
    floor->AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f)); // top surface at y = 0.5

    // Resting spheres, 1.5 apart so they only touch the floor; x deliberately not in creation order
    const float xs[] = {3.0f, -3.0f, 0.0f, -4.5f, 1.5f, -1.5f};
    std::vector<GameObject::Ptr> balls;
    std::vector<std::string> created;
    for (int i = 0; i < 6; ++i)
    {
        const std::string name = "Ball" + std::to_string(i);
        const auto ball = Spawn(name, Vector3(xs[i], 1.05f, 0.0f), 0);
        ball->AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = ball->AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(true);
        ball->AddComponent<ExitRecorder>();
        balls.push_back(ball);
        created.push_back(name);
    }

    Step(120); // settle onto the floor
    g_exits.clear();

    floor->Destroy();
    Step(3);

    // Each ball loses its touch with the floor exactly once, in creation order
    EXPECT_EQ(g_exits, created);
}

#endif // N2ENGINE_PHYSX_ENABLED
