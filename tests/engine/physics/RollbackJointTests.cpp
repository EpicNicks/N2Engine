// Joint components through the rollback world (stage c of #381 item 6): a chain of bodies held together by Joint
// components of every type is saved with RollbackWorld::Save, run on, restored with Restore and run again, and must end
// with the same physics hashes (Jolt's HashState and HashSimulatedState, which cover the joints) and the same replicated
// fields as a run that never rolled back. The Joint components are not rolled back themselves: they keep the handles of
// the backend's joints, which stay valid because the joint set did not change inside the window.

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../network/NetworkTestSupport.hpp"

#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/physics/Joint.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        using namespace N2Engine::Network;
        using namespace N2Engine::Network::Testing;

        constexpr float kStep = 1.0f / 60.0f;

        struct Outcome
        {
            std::optional<std::uint64_t> physics;
            std::optional<std::uint64_t> simulated;
            std::uint64_t root = 0;
            std::string fields;

            friend bool operator==(const Outcome &, const Outcome &) = default;
        };

        // Five balls in a row, each joined to the next by a Joint component (Ball, Hinge, Distance, Slider), the first
        // also held to the world, over a floor: they swing, collide and get impulses
        class JointGame
        {
        public:
            JointGame()
            {
                SceneManager::AddScene(Scene::Create("RbJoints"), true);
                SceneManager::ProcessAnyPendingSceneChange();
                scene = SceneManager::GetCurScene();

                const GameObject::Ptr floor = GameObject::Create("Floor");
                floor->CreatePositionable();
                floor->AddComponent<BoxCollider>()->SetSize(Vector3(30.0f, 1.0f, 30.0f));
                scene->AddRootGameObject(floor);

                for (std::uint32_t i = 0; i < 5; ++i)
                {
                    const GameObject::Ptr ball = GameObject::Create("Link" + std::to_string(i));
                    ball->SetUUID(TestUuid(10 + i));
                    ball->CreatePositionable();
                    ball->GetPositionable()->SetPosition(Vector3(1.2f * static_cast<float>(i), 4.0f, 0.0f));
                    ball->AddComponent<NetworkObject>();
                    ball->AddComponent<NetworkTransform>();
                    ball->AddComponent<SphereCollider>()->SetRadius(0.4f);
                    Rigidbody *const body = ball->AddComponent<Rigidbody>();
                    body->SetBodyType(BodyType::Dynamic);
                    body->SetGravityEnabled(true);
                    scene->AddRootGameObject(ball);
                    balls.push_back(ball.get());
                    bodies.push_back(body);
                }
                const JointType types[4] = {JointType::Ball, JointType::Hinge, JointType::Distance, JointType::Slider};
                for (std::size_t i = 0; i < 4; ++i)
                {
                    Joint *const joint = balls[i]->AddComponent<Joint>();
                    joint->SetJointType(types[i]);
                    joint->SetConnectedBody(balls[i + 1]);
                    joint->SetAnchor(Vector3(0.6f, 0.0f, 0.0f));
                    joint->SetConnectedAnchor(Vector3(-0.6f, 0.0f, 0.0f));
                    joint->SetAxis(Vector3(0.0f, 0.0f, 1.0f));
                    if (types[i] == JointType::Hinge)
                    {
                        joint->SetLimits(true, -60.0f, 60.0f);
                    }
                    else if (types[i] == JointType::Distance)
                    {
                        joint->SetLimits(true, 0.5f, 1.6f);
                    }
                    else if (types[i] == JointType::Slider)
                    {
                        joint->SetAxis(Vector3(1.0f, 0.0f, 0.0f));
                        joint->SetLimits(true, -0.3f, 0.3f);
                    }
                    joints.push_back(joint);
                }
                Joint *const anchor = balls[0]->AddComponent<Joint>();
                anchor->SetJointType(JointType::Ball);
                anchor->SetAnchor(Vector3(-0.4f, 0.0f, 0.0f));
                anchor->SetConnectedAnchor(Vector3(-0.4f, 4.0f, 0.0f)); // the world point
                joints.push_back(anchor);

                EXPECT_TRUE(world.BindScene(*scene).has_value());
                scene->ProcessAttachQueue();
            }

            ~JointGame()
            {
                SceneManager::AddScene(Scene::Create("RbJoints_Teardown"), true);
                SceneManager::ProcessAnyPendingSceneChange();
                Application::GetInstance().Set3DPhysicsBackend(nullptr);
            }

            void Logic(const std::uint32_t tick)
            {
                if (tick % 5 == 3)
                {
                    bodies[tick / 5 % bodies.size()]->AddImpulse(Vector3(1.5f, 1.0f, 0.5f * static_cast<float>(tick % 3)));
                }
            }

            void Step(const std::uint32_t tick)
            {
                Logic(tick);
                scene->ProcessAttachQueue();
                rollback.CommitSpawns();
                backend->ApplyPendingChanges();
                scene->FixedUpdate(); // a Joint makes its joint here, once both bodies exist
                backend->Update(kStep);
                backend->SyncTransforms();
                backend->ProcessCollisionCallbacks();
                scene->ProcessDestroyed();
            }

            void Run(const std::uint32_t from, const std::uint32_t to)
            {
                for (std::uint32_t tick = from; tick < to; ++tick)
                {
                    Step(tick);
                }
            }

            Outcome Capture(const std::uint32_t tick)
            {
                Outcome outcome;
                outcome.physics = backend->HashState();
                outcome.simulated = backend->HashSimulatedState();
                const auto tree = BuildStateHashTree(world, rollback.Layouts(), tick);
                EXPECT_TRUE(tree.has_value());
                outcome.root = tree.has_value() ? tree->Root() : 0;
                for (const auto &entry : world.Objects())
                {
                    GameObject &gameObject = entry.second->GetGameObject();
                    const ObjectValues values = CaptureObjectValues(rollback.Layouts().Get(NetworkId{entry.first}, gameObject), gameObject);
                    outcome.fields += "#" + std::to_string(entry.first) + ":";
                    for (const auto &field : values.fields)
                    {
                        outcome.fields += field.has_value() ? field->dump() : std::string("null");
                        outcome.fields += ",";
                    }
                }
                return outcome;
            }

            static JoltBackend *InstallBackend()
            {
                auto created = std::make_unique<JoltBackend>();
                created->SetDeterministic(true);
                EXPECT_TRUE(created->Initialize());
                JoltBackend *const raw = created.get();
                Application::GetInstance().Set3DPhysicsBackend(std::move(created));
                return raw;
            }

            JoltBackend *backend = InstallBackend();
            Scene *scene = nullptr;
            std::vector<GameObject *> balls;
            std::vector<Rigidbody *> bodies;
            std::vector<Joint *> joints;
            NetworkWorld world;
            RollbackWorld rollback{world, backend};
        };
    }

    TEST(RollbackJointTest, ARollbackOfAJointedChainEndsWithTheStraightRunsPhysicsHashAndFields)
    {
        std::vector<Outcome> straight(41);
        {
            JointGame game;
            for (std::uint32_t tick = 0; tick < 40; ++tick)
            {
                game.Step(tick);
                straight[tick + 1] = game.Capture(tick + 1);
            }
            for (const Joint *joint : game.joints)
            {
                EXPECT_TRUE(joint->GetHandle().IsValid());
            }
        }
        ASSERT_NE(straight[10], straight[30]);

        JointGame game;
        game.Run(0, 10);
        WorldSnapshot at10;
        ASSERT_TRUE(game.rollback.Save(10, at10));
        EXPECT_TRUE(at10.hasPhysics);
        ASSERT_EQ(game.Capture(10), straight[10]);

        for (const std::uint32_t reach : {11u, 14u, 25u})
        {
            game.Run(10, reach);
            const auto restored = game.rollback.Restore(at10);
            ASSERT_TRUE(restored.has_value()) << restored.error();
            const Outcome restored10 = game.Capture(10);
            EXPECT_EQ(restored10.root, straight[10].root);
            EXPECT_EQ(restored10.fields, straight[10].fields);
            EXPECT_EQ(restored10.simulated, straight[10].simulated);
            EXPECT_EQ(restored10.physics, straight[10].physics) << "HashState, constraint state included";
            for (std::uint32_t tick = 10; tick < 40; ++tick)
            {
                game.Step(tick);
                ASSERT_EQ(game.Capture(tick + 1), straight[tick + 1]) << "reach " << reach << ", tick " << tick + 1;
            }
            // back to the start of the next round: restore again
            ASSERT_TRUE(game.rollback.Restore(at10).has_value());
        }
        for (const Joint *joint : game.joints)
        {
            EXPECT_TRUE(game.backend->IsJointValid(joint->GetHandle())) << "a restore must leave the components' joints alive";
        }
    }
}

#endif // N2ENGINE_JOLT_ENABLED
