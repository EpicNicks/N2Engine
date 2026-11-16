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
#include "engine/prefabs/PrefabManager.hpp"
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

    // ===== The joint set changes inside the rollback window (stage d) =====
    //
    // A Joint component made, destroyed, disabled or re-enabled inside the window, and a jointed object spawned or
    // despawned there (despawn disables it), roll back through RollbackWorld Save/Restore. The components themselves are
    // game state: the game's logic is a function of the tick that sets them every tick (adds or removes the component,
    // enables or disables it, spawns or despawns), so after a Restore it brings the components to the state of the tick
    // it resumes from. The backend puts the joint SET back from the snapshot and a component takes back its joint.

    enum class Change
    {
        Create,  // the joint's component is added at tick `on`
        Destroy, // the joint's component is removed at tick `on`
        Disable, // the component is disabled at `on` and enabled again at `off`
        Spawn    // a jointed object is spawned at `on` and despawned at `off`
    };

    struct Schedule
    {
        Change change = Change::Create;
        std::uint32_t on = 12;
        std::uint32_t off = 20;
    };

    constexpr const char *kJointedPrefab = "RbJointedBall";

    class ScopedJointedPrefab
    {
    public:
        ScopedJointedPrefab()
        {
            const GameObject::Ptr root = GameObject::Create("JointedBall");
            root->CreatePositionable();
            root->AddComponent<NetworkObject>();
            root->AddComponent<NetworkTransform>();
            root->AddComponent<SphereCollider>()->SetRadius(0.4f);
            Rigidbody *const body = root->AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(true);
            Joint *const joint = root->AddComponent<Joint>(); // a rope to a point of the world above the spawn point
            joint->SetJointType(JointType::Distance);
            joint->SetAnchor(Vector3(0.0f, 0.4f, 0.0f));
            joint->SetConnectedAnchor(Vector3(6.0f, 7.0f, 0.0f));
            joint->SetLimits(true, 0.5f, 2.0f);
            EXPECT_TRUE(PrefabManager::RegisterPrefab(kJointedPrefab, root));
        }
        ~ScopedJointedPrefab() { PrefabManager::UnregisterPrefab(kJointedPrefab); }
        ScopedJointedPrefab(const ScopedJointedPrefab &) = delete;
        ScopedJointedPrefab &operator=(const ScopedJointedPrefab &) = delete;
    };

    // Four balls in a row over a floor. Link0 is joined to Link1 and to the world for the whole run; Link2 and Link3 are
    // the ones whose Joint component the schedule changes (a limited hinge), or a jointed ball is spawned beside them.
    class LifecycleGame
    {
    public:
        explicit LifecycleGame(const Schedule &initial) : schedule(initial)
        {
            SceneManager::AddScene(Scene::Create("RbJointLife"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            scene = SceneManager::GetCurScene();

            const GameObject::Ptr floor = GameObject::Create("Floor");
            floor->CreatePositionable();
            floor->AddComponent<BoxCollider>()->SetSize(Vector3(30.0f, 1.0f, 30.0f));
            scene->AddRootGameObject(floor);

            for (std::uint32_t i = 0; i < 4; ++i)
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
            Joint *const chain = balls[0]->AddComponent<Joint>();
            chain->SetJointType(JointType::Ball);
            chain->SetConnectedBody(balls[1]);
            chain->SetAnchor(Vector3(0.6f, 0.0f, 0.0f));
            chain->SetConnectedAnchor(Vector3(-0.6f, 0.0f, 0.0f));
            Joint *const anchor = balls[0]->AddComponent<Joint>();
            anchor->SetJointType(JointType::Ball);
            anchor->SetAnchor(Vector3(-0.4f, 0.0f, 0.0f));
            anchor->SetConnectedAnchor(Vector3(-0.4f, 4.0f, 0.0f));
            if (schedule.change == Change::Disable)
            {
                ConfigureVariable(*balls[2]->AddComponent<Joint>());
            }
            EXPECT_TRUE(world.BindScene(*scene).has_value());
            scene->ProcessAttachQueue();
        }

        ~LifecycleGame()
        {
            SceneManager::AddScene(Scene::Create("RbJointLife_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        void ConfigureVariable(Joint &joint) const
        {
            joint.SetJointType(JointType::Hinge);
            joint.SetConnectedBody(balls[3]);
            joint.SetAnchor(Vector3(0.6f, 0.0f, 0.0f));
            joint.SetConnectedAnchor(Vector3(-0.6f, 0.0f, 0.0f));
            joint.SetAxis(Vector3(0.0f, 0.0f, 1.0f));
            joint.SetLimits(true, -60.0f, 60.0f);
        }

        Joint *Variable() const
        {
            Joint *const joint = balls[2]->GetComponent<Joint>();
            return joint != nullptr && !joint->IsDestroyed() ? joint : nullptr;
        }

        /// The game's logic of one tick: a function of the tick alone, applied to whatever state the game is in
        void Logic(const std::uint32_t tick)
        {
            if (tick % 5 == 3)
            {
                bodies[tick / 5 % bodies.size()]->AddImpulse(Vector3(1.5f, 1.0f, 0.5f * static_cast<float>(tick % 3)));
            }
            switch (schedule.change)
            {
            case Change::Create:
            case Change::Destroy:
            {
                const bool want = schedule.change == Change::Create ? tick >= schedule.on : tick < schedule.on;
                Joint *const joint = Variable();
                if (want && joint == nullptr)
                {
                    ConfigureVariable(*balls[2]->AddComponent<Joint>());
                }
                else if (!want && joint != nullptr)
                {
                    balls[2]->RemoveComponent(joint);
                }
                break;
            }
            case Change::Disable:
                if (Joint *const joint = Variable())
                {
                    joint->SetActive(!(tick >= schedule.on && tick < schedule.off));
                }
                break;
            case Change::Spawn:
                if (tick == schedule.on)
                {
                    const auto spawned = rollback.Spawn(kJointedPrefab, kServerClientId, tick);
                    ASSERT_TRUE(spawned.has_value());
                    (*spawned)->GetGameObject().GetPositionable()->SetPosition(Vector3(6.0f, 5.0f, 0.0f));
                }
                if (tick == schedule.off)
                {
                    for (const auto &entry : world.Objects())
                    {
                        if (entry.second->IsSpawned())
                        {
                            EXPECT_TRUE(rollback.Despawn(NetworkId{entry.first}, tick));
                            break;
                        }
                    }
                }
                break;
            }
        }

        void Step(const std::uint32_t tick)
        {
            Logic(tick);
            scene->ProcessAttachQueue();
            rollback.CommitSpawns();
            backend->ApplyPendingChanges();
            scene->FixedUpdate();
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
            if (schedule.change == Change::Spawn && tick <= schedule.on)
            {
                // HashState covers every body slot, parked ones too: a world that undid the spawn still holds the
                // spawned object's parked body, which a straight run has not made yet. (The simulated hash does not.)
                outcome.physics.reset();
            }
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

        /// How many of the objects' Joint components hold a live backend joint right now
        std::size_t LiveJoints() const
        {
            std::size_t count = 0;
            for (const auto &entry : world.Objects())
            {
                for (const Joint *joint : entry.second->GetGameObject().GetComponents<Joint>())
                {
                    count += joint != nullptr && backend->IsJointValid(joint->GetHandle()) ? 1 : 0;
                }
            }
            return count;
        }

        /// How many joints the schedule says exist after `tick` steps have run
        std::size_t ExpectedJoints(const std::uint32_t tick) const
        {
            const std::uint32_t last = tick - 1; // the tick of the last step
            switch (schedule.change)
            {
            case Change::Create: return 2 + (last >= schedule.on ? 1 : 0);
            case Change::Destroy: return 2 + (last < schedule.on ? 1 : 0);
            case Change::Disable: return 2 + (last >= schedule.on && last < schedule.off ? 0 : 1);
            case Change::Spawn: return 2 + (last >= schedule.on && last < schedule.off ? 1 : 0);
            }
            return 0;
        }

        /// The physics bytes of a snapshot at `tick`
        std::vector<std::byte> PhysicsBytes(const std::uint32_t tick)
        {
            WorldSnapshot snapshot;
            EXPECT_TRUE(rollback.Save(tick, snapshot));
            return snapshot.physics;
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

        Schedule schedule;
        JoltBackend *backend = InstallBackend();
        Scene *scene = nullptr;
        std::vector<GameObject *> balls;
        std::vector<Rigidbody *> bodies;
        NetworkWorld world;
        RollbackWorld rollback{world, backend};
    };

    constexpr std::uint32_t kLifeTicks = 44;

    struct StraightRun
    {
        std::vector<Outcome> outcomes; // by tick, 0..kLifeTicks
        std::vector<std::byte> finalPhysics;
    };

    StraightRun RunStraight(const Schedule &schedule)
    {
        ScopedJointedPrefab prefab;
        LifecycleGame game(schedule);
        StraightRun run;
        run.outcomes.push_back(game.Capture(0));
        for (std::uint32_t tick = 0; tick < kLifeTicks; ++tick)
        {
            game.Step(tick);
            EXPECT_EQ(game.LiveJoints(), game.ExpectedJoints(tick + 1)) << "the straight run's joints after tick " << tick;
            run.outcomes.push_back(game.Capture(tick + 1));
        }
        run.finalPhysics = game.PhysicsBytes(kLifeTicks);
        return run;
    }

    // Save at `saveTick`, run on to `reach` (the change happens in between), restore, resume to the end: the state hash
    // equals the straight run's at every tick and the final snapshot is byte for byte the straight run's. Twice over, from
    // the same snapshot.
    void ExpectRollbackEqualsStraight(const Schedule &schedule, const std::uint32_t saveTick, const std::uint32_t reach)
    {
        SCOPED_TRACE("change " + std::to_string(static_cast<int>(schedule.change)) + " on " + std::to_string(schedule.on) +
                     " off " + std::to_string(schedule.off) + " save " + std::to_string(saveTick) + " reach " +
                     std::to_string(reach));
        const StraightRun straight = RunStraight(schedule);
        ASSERT_NE(straight.outcomes[saveTick], straight.outcomes[kLifeTicks]);

        ScopedJointedPrefab prefab;
        LifecycleGame game(schedule);
        game.Run(0, saveTick);
        WorldSnapshot saved;
        ASSERT_TRUE(game.rollback.Save(saveTick, saved));
        ASSERT_EQ(game.Capture(saveTick), straight.outcomes[saveTick]);

        for (int round = 0; round < 2; ++round)
        {
            game.Run(saveTick, reach);
            const auto restored = game.rollback.Restore(saved);
            ASSERT_TRUE(restored.has_value()) << restored.error();
            {
                const Outcome now = game.Capture(saveTick);
                const Outcome &want = straight.outcomes[saveTick];
                EXPECT_EQ(now.physics, want.physics) << "physics hash after the restore";
                EXPECT_EQ(now.simulated, want.simulated) << "simulated hash after the restore";
                EXPECT_EQ(now.root, want.root) << "session hash root after the restore";
                EXPECT_EQ(now.fields, want.fields) << "replicated fields after the restore";
                ASSERT_EQ(now, want) << "round " << round << ": the restore itself";
            }
            for (std::uint32_t tick = saveTick; tick < kLifeTicks; ++tick)
            {
                game.Step(tick);
                ASSERT_EQ(game.LiveJoints(), game.ExpectedJoints(tick + 1)) << "the components' joints, round " << round << ", tick " << tick + 1;
                ASSERT_EQ(game.Capture(tick + 1), straight.outcomes[tick + 1]) << "round " << round << ", tick " << tick + 1;
            }
            EXPECT_EQ(game.PhysicsBytes(kLifeTicks), straight.finalPhysics) << "round " << round << ": the final snapshot";
            if (round == 0)
            {
                // the snapshot is still good for another go, even from the end state
                game.Run(kLifeTicks, kLifeTicks + 3);
                ASSERT_TRUE(game.rollback.Restore(saved).has_value());
            }
        }
    }

    TEST(RollbackJointLifecycleTest, AJointComponentCreatedInsideTheWindowRollsBack)
    {
        ExpectRollbackEqualsStraight({Change::Create, 12, 0}, 8, 20);
        ExpectRollbackEqualsStraight({Change::Create, 12, 0}, 8, 10); // restored before it was made
    }

    TEST(RollbackJointLifecycleTest, AJointComponentDestroyedInsideTheWindowRollsBack)
    {
        ExpectRollbackEqualsStraight({Change::Destroy, 12, 0}, 8, 20);
        ExpectRollbackEqualsStraight({Change::Destroy, 12, 0}, 8, 11);
    }

    TEST(RollbackJointLifecycleTest, AJointComponentDisabledAndEnabledAgainInsideTheWindowRollsBack)
    {
        ExpectRollbackEqualsStraight({Change::Disable, 10, 20}, 6, 26); // both inside the window
        ExpectRollbackEqualsStraight({Change::Disable, 10, 20}, 6, 14); // disabled inside it
        ExpectRollbackEqualsStraight({Change::Disable, 10, 20}, 14, 26); // disabled at the save, enabled inside the window
    }

    TEST(RollbackJointLifecycleTest, AJointedObjectSpawnedAndDespawnedInsideTheWindowRollsBack)
    {
        ExpectRollbackEqualsStraight({Change::Spawn, 8, 24}, 4, 14);  // spawned inside the window
        ExpectRollbackEqualsStraight({Change::Spawn, 8, 24}, 4, 30);  // spawned and despawned inside it
        ExpectRollbackEqualsStraight({Change::Spawn, 8, 24}, 14, 30); // alive at the save, despawned inside the window
    }

    // A world with another joint history takes the snapshot of this one: the game puts its schedule back to the snapshot
    // timeline, and the run on equals the straight run
    void ExpectRestoreIntoAnotherHistory(const Schedule &saveSchedule, const Schedule &otherSchedule, const std::uint32_t saveTick,
                                         const std::uint32_t otherReach)
    {
        SCOPED_TRACE("change " + std::to_string(static_cast<int>(saveSchedule.change)));
        const StraightRun straight = RunStraight(saveSchedule);
        ScopedJointedPrefab prefab;
        WorldSnapshot saved;
        {
            LifecycleGame first(saveSchedule);
            first.Run(0, saveTick);
            ASSERT_TRUE(first.rollback.Save(saveTick, saved));
        }
        LifecycleGame other(otherSchedule);
        other.Run(0, otherReach);
        other.schedule = saveSchedule;
        const auto restored = other.rollback.Restore(saved);
        ASSERT_TRUE(restored.has_value()) << restored.error();
        ASSERT_EQ(other.Capture(saveTick), straight.outcomes[saveTick]);
        for (std::uint32_t tick = saveTick; tick < kLifeTicks; ++tick)
        {
            other.Step(tick);
            ASSERT_EQ(other.Capture(tick + 1), straight.outcomes[tick + 1]) << "tick " << tick + 1;
        }
        EXPECT_EQ(other.PhysicsBytes(kLifeTicks), straight.finalPhysics);
    }

    TEST(RollbackJointLifecycleTest, ASnapshotRestoresIntoAWorldWithAnotherJointHistory)
    {
        ExpectRestoreIntoAnotherHistory({Change::Create, 16, 0}, {Change::Create, 5, 0}, 8, 26);
        ExpectRestoreIntoAnotherHistory({Change::Destroy, 16, 0}, {Change::Destroy, 5, 0}, 8, 26);
        ExpectRestoreIntoAnotherHistory({Change::Disable, 10, 24}, {Change::Disable, 4, 8}, 6, 26);
        ExpectRestoreIntoAnotherHistory({Change::Spawn, 8, 28}, {Change::Spawn, 3, 9}, 4, 20);
    }
}

#endif // N2ENGINE_JOLT_ENABLED
