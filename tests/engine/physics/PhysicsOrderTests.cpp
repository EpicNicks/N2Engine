// Iteration order of the backends' re-filtering and touch maps: by body creation, not by address or hash. TYPED tests
// over every backend this build has (PhysX and Jolt); the touch maps are the ContactTracker's, shared by both.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/Layers.hpp"

namespace N2PhysicsTests
{
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

    template <typename Tag>
    class PhysicsOrderTest : public ::testing::Test
    {
    protected:
        using Backend = typename Tag::Backend;

        Scene *_scene = nullptr;

        // A fresh backend per test: handle indices come from a free list, so a shared backend would hand the next
        // test's bodies recycled (descending) indices and the creation-order expectations would not hold.
        void SetUp() override
        {
            auto backend = std::make_unique<Backend>();
            ASSERT_TRUE(backend->Initialize());
            Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
            Layers::ResetToDefaults();
            SceneManager::AddScene(Scene::Create(std::string("PhysicsOrder_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()),
                                   true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
            g_exits.clear();
        }

        void TearDown() override
        {
            Native().SetRefilterObserver({});
            Layers::ResetToDefaults();
            SceneManager::AddScene(Scene::Create("PhysicsOrderTest_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        static IPhysicsBackend &BackendRef() { return *Application::GetInstance().Get3DPhysicsBackend(); }
        static Backend &Native() { return static_cast<Backend &>(BackendRef()); }

        void Step(const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                BackendRef().ApplyPendingChanges();
                _scene->FixedUpdate();
                BackendRef().Update(1.0f / 60.0f);
                BackendRef().SyncTransforms();
                BackendRef().ProcessCollisionCallbacks();
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr SpawnOnLayer(const std::string &name, const Vector3 &position, const int layer)
        {
            const auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(position);
            go->SetLayer(layer);
            _scene->AddRootGameObject(go);
            return go;
        }
    };

    TYPED_TEST_SUITE(PhysicsOrderTest, AllBackends, BackendNames);

    // Layers::SetCollision re-filters every shape whose row changed. The bodies must be visited in the order they
    // were created whatever order their colliders were allocated in or their positions suggest.
    TYPED_TEST(PhysicsOrderTest, CollisionMatrixRefilterVisitsBodiesInCreationOrder)
    {
        constexpr int kBodies = 12;
        std::vector<GameObject::Ptr> objects;
        std::vector<std::string> created;
        for (int i = 0; i < kBodies; ++i)
        {
            // Positions deliberately not in creation order; far apart so nothing touches
            const float x = static_cast<float>((i * 7) % kBodies) * 10.0f;
            const std::string name = "Static" + std::to_string(i);
            objects.push_back(this->SpawnOnLayer(name, Vector3(x, 0.0f, 0.0f), LayerA));
            objects.back()->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            created.push_back(name);
        }
        this->Step(2);

        std::vector<std::string> visited;
        this->Native().SetRefilterObserver([&visited](const GameObject &owner) { visited.push_back(owner.GetName()); });

        Layers::SetCollision(LayerA, LayerA, false); // changes the collision row of every shape on LayerA

        EXPECT_EQ(visited, created);
    }

    // A body's destruction ends its touches; the Exit events go out in the touch map's order, which is by body
    // creation (the floor is the oldest body, so the pairs sort by the other body).
    TYPED_TEST(PhysicsOrderTest, ForgottenTouchesEndInCreationOrder)
    {
        const auto floor = this->SpawnOnLayer("Floor", Vector3(0.0f, 0.0f, 0.0f), 0);
        floor->template AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f)); // top surface at y = 0.5

        // Resting spheres, 1.5 apart so they only touch the floor; x deliberately not in creation order
        const float xs[] = {3.0f, -3.0f, 0.0f, -4.5f, 1.5f, -1.5f};
        std::vector<GameObject::Ptr> balls;
        std::vector<std::string> created;
        for (int i = 0; i < 6; ++i)
        {
            const std::string name = "Ball" + std::to_string(i);
            const auto ball = this->SpawnOnLayer(name, Vector3(xs[i], 1.05f, 0.0f), 0);
            ball->template AddComponent<SphereCollider>()->SetRadius(0.5f);
            auto *body = ball->template AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(true);
            ball->template AddComponent<ExitRecorder>();
            balls.push_back(ball);
            created.push_back(name);
        }

        this->Step(120); // settle onto the floor
        g_exits.clear();

        floor->Destroy();
        this->Step(3);

        // Each ball loses its touch with the floor exactly once, in creation order
        EXPECT_EQ(g_exits, created);
    }
}

#endif // a physics backend is in this build
