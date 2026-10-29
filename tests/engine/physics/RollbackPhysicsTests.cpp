// Netcode C5, the physics part of the rollback world: a scene with Jolt bodies is saved, run on, restored and run again,
// and must end with the same physics hash and the same replicated fields as a run that never rolled back. It covers the
// parked-body contract end to end (a body spawned after the restore point, a body despawned after it) and the pooled
// reuse of a spawned body (same handle, so the same hash).

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../network/NetworkTestSupport.hpp"
#include <map>

#include "engine/network/InputLayout.hpp"
#include "engine/network/LockstepRelay.hpp"
#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/RollbackRuntime.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        using namespace N2Engine::Network;
        using namespace N2Engine::Network::Testing;

        constexpr const char *kBallPrefab = "RbPhysBall";
        constexpr float kStep = 1.0f / 60.0f;

        constexpr const char *kPairPrefab = "RbPhysPair";

        struct Outcome
        {
            std::optional<std::uint64_t> physics;
            /// HashSimulatedState: parked bodies skipped, handle numbers not hashed
            std::optional<std::uint64_t> simulated;
            std::uint64_t root = 0;
            std::string fields;

            friend bool operator==(const Outcome &, const Outcome &) = default;
        };

        /// A Jolt world in the current scene: a floor, two dynamic balls that are scene objects (net ids 1 and 2) and a
        /// prefab of the same ball for spawning. Each Game has its own backend so two runs start from the same handles.
        class Game
        {
        public:
            Game()
            {
                SceneManager::AddScene(Scene::Create("RbPhysics"), true);
                SceneManager::ProcessAnyPendingSceneChange();
                scene = SceneManager::GetCurScene();

                const GameObject::Ptr prefabRoot = GameObject::Create("BallPrefab");
                prefabRoot->CreatePositionable();
                prefabRoot->AddComponent<NetworkObject>();
                prefabRoot->AddComponent<NetworkTransform>();
                prefabRoot->AddComponent<SphereCollider>()->SetRadius(0.5f);
                Rigidbody *const body = prefabRoot->AddComponent<Rigidbody>();
                body->SetBodyType(BodyType::Dynamic);
                body->SetGravityEnabled(true);
                EXPECT_TRUE(PrefabManager::RegisterPrefab(kBallPrefab, prefabRoot));

                // A prefab whose Rigidbody is on a CHILD (the root only carries the net identity and its transform)
                const GameObject::Ptr pairRoot = GameObject::Create("PairPrefab");
                pairRoot->CreatePositionable();
                pairRoot->AddComponent<NetworkObject>();
                pairRoot->AddComponent<NetworkTransform>();
                const GameObject::Ptr pairChild = GameObject::Create("PairBody");
                pairChild->CreatePositionable();
                pairChild->GetPositionable()->SetLocalPosition(Vector3(0.0f, 0.75f, 0.0f));
                pairChild->AddComponent<SphereCollider>()->SetRadius(0.4f);
                Rigidbody *const childBody = pairChild->AddComponent<Rigidbody>();
                childBody->SetBodyType(BodyType::Dynamic);
                childBody->SetGravityEnabled(true);
                pairRoot->AddChild(pairChild);
                EXPECT_TRUE(PrefabManager::RegisterPrefab(kPairPrefab, pairRoot));

                const GameObject::Ptr floor = GameObject::Create("Floor");
                floor->CreatePositionable();
                floor->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, 0.0f));
                floor->AddComponent<BoxCollider>()->SetSize(Vector3(30.0f, 1.0f, 30.0f));
                scene->AddRootGameObject(floor);

                AddBall(10, Vector3(0.0f, 3.0f, 0.0f));
                AddBall(20, Vector3(1.5f, 4.0f, 0.2f));
                EXPECT_TRUE(world.BindScene(*scene).has_value());
                scene->ProcessAttachQueue();
            }

            ~Game()
            {
                // Leave no loaded scene holding bodies of a backend that is about to go away
                SceneManager::AddScene(Scene::Create("RbPhysics_Teardown"), true);
                SceneManager::ProcessAnyPendingSceneChange();
                Application::GetInstance().Set3DPhysicsBackend(nullptr);
                PrefabManager::UnregisterPrefab(kBallPrefab);
                PrefabManager::UnregisterPrefab(kPairPrefab);
            }

            void AddBall(const std::uint32_t number, const Vector3 &position)
            {
                const GameObject::Ptr ball = GameObject::Create("Ball" + std::to_string(number));
                ball->SetUUID(TestUuid(number));
                ball->CreatePositionable();
                ball->GetPositionable()->SetPosition(position);
                ball->AddComponent<NetworkObject>();
                ball->AddComponent<NetworkTransform>();
                ball->AddComponent<SphereCollider>()->SetRadius(0.5f);
                Rigidbody *const body = ball->AddComponent<Rigidbody>();
                body->SetBodyType(BodyType::Dynamic);
                body->SetGravityEnabled(true);
                scene->AddRootGameObject(ball);
            }

            /// The simulation's own decisions for a tick: impulses, a despawn and two spawns
            void Logic(const std::uint32_t tick) { Logic(tick, rollback); }

            /// The same, spawning and despawning through `rb` (a RollbackRuntime's world, when a session drives the game)
            void Logic(const std::uint32_t tick, RollbackWorld &rb)
            {
                if (tick == 3)
                {
                    world.Find(NetworkId{1})->GetGameObject().GetComponent<Rigidbody>()->AddImpulse(Vector3(2.0f, 0.0f, 0.0f));
                }
                if (tick == 5 || tick == 12)
                {
                    const auto spawned = rb.Spawn(tick == 5 ? spawnPrefab : kBallPrefab, 0, tick);
                    ASSERT_TRUE(spawned.has_value()) << spawned.error();
                    GameObject &ball = (*spawned)->GetGameObject();
                    if (ball.GetPositionable() == nullptr)
                    {
                        ball.CreatePositionable();
                    }
                    ball.GetPositionable()->SetPosition(tick == 5 ? Vector3(0.3f, 5.0f, 0.1f) : Vector3(-1.0f, 6.0f, 0.0f));
                }
                if (tick == 8)
                {
                    EXPECT_TRUE(rb.Despawn(NetworkId{2}, tick));
                }
                if (tick == 9)
                {
                    world.Find(NetworkId{1})->GetGameObject().GetComponent<Rigidbody>()->AddImpulse(Vector3(0.0f, 4.0f, 0.0f));
                }
            }

            void Step(const std::uint32_t tick)
            {
                Logic(tick);
                scene->ProcessAttachQueue(); // a fresh instance gets its body here
                rollback.CommitSpawns();     // a pooled one is activated here
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
                const auto tree = BuildStateHashTree(world, rollback.Layouts(), tick);
                EXPECT_TRUE(tree.has_value());
                outcome.root = tree.has_value() ? tree->Root() : 0;
                for (const auto &entry : world.Objects())
                {
                    GameObject &gameObject = entry.second->GetGameObject();
                    const ObjectValues values = CaptureObjectValues(rollback.Layouts().Get(NetworkId{entry.first}, gameObject), gameObject);
                    outcome.fields += "#" + std::to_string(entry.first) + (gameObject.IsActive() ? "a:" : "i:");
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
            /// What tick 5 spawns (tick 12 always spawns a ball)
            const char *spawnPrefab = kBallPrefab;
            Scene *scene = nullptr;
            NetworkWorld world;
            RollbackWorld rollback{world, backend};
        };
    }

    TEST(RollbackPhysicsTest, ARollbackAcrossSpawnsDespawnsAndImpulsesEndsWithTheStraightRunsPhysicsHash)
    {
        Outcome straight4;
        Outcome straight10;
        Outcome straight14;
        Outcome straight20;
        std::uint64_t straight4Tree = 0;
        {
            Game game;
            game.Run(0, 4);
            straight4 = game.Capture(4);
            straight4Tree = BuildStateHashTree(game.world, game.rollback.Layouts(), 4, game.backend)->Root();
            game.Run(4, 10);
            straight10 = game.Capture(10);
            game.Run(10, 14);
            straight14 = game.Capture(14);
            game.Run(14, 20);
            straight20 = game.Capture(20);
        }
        ASSERT_TRUE(straight20.physics.has_value());
        ASSERT_NE(straight4, straight10);
        ASSERT_NE(straight10, straight20);

        Game game;
        game.Run(0, 4);
        WorldSnapshot at4;
        ASSERT_TRUE(game.rollback.Save(4, at4));
        EXPECT_TRUE(at4.hasPhysics);
        ASSERT_EQ(game.Capture(4), straight4);

        game.Run(4, 10); // a spawn (tick 5), a despawn (tick 8) and two impulses happen
        ASSERT_EQ(game.Capture(10), straight10);
        EXPECT_EQ(game.world.Count(), 2u) << "ball 1 and the spawned one (id 3); ball 2 was despawned";
        EXPECT_EQ(game.world.Parked().size(), 1u);

        // Rewind across all of it
        const auto restored = game.rollback.Restore(at4);
        ASSERT_TRUE(restored.has_value()) << restored.error();
        {
            // The replicated state is exactly the saved one. Jolt's HashState also counts a PARKED body (the spawned
            // ball is parked and waiting in the pool), which the straight run never created, so that hash is compared
            // again once the resimulation has used the pool (tick 10). HashSimulatedState, the one sessions exchange,
            // skips the parked body and matches at once.
            const Outcome restored4 = game.Capture(4);
            EXPECT_EQ(restored4.root, straight4.root);
            EXPECT_EQ(restored4.fields, straight4.fields);
            ASSERT_TRUE(restored4.simulated.has_value());
            EXPECT_EQ(restored4.simulated, straight4.simulated);
            EXPECT_NE(restored4.physics, straight4.physics) << "HashState still counts the parked body";
            // The physics leaf of the session tree agrees too
            const auto restoredTree = BuildStateHashTree(game.world, game.rollback.Layouts(), 4, game.backend);
            ASSERT_TRUE(restoredTree.has_value());
            EXPECT_EQ(restoredTree->Root(), straight4Tree);
        }
        EXPECT_EQ(game.world.Count(), 2u);
        EXPECT_GE(game.rollback.Stats().spawnsUndone, 1u);
        EXPECT_GE(game.rollback.Stats().despawnsUndone, 1u);

        game.Run(4, 10);
        EXPECT_GE(game.rollback.Stats().spawnsPooled, 1u) << "the resimulated spawn took the parked body back";
        EXPECT_EQ(game.Capture(10), straight10);

        // A second rollback, shorter, with a snapshot taken during the resimulation
        game.Run(10, 14);
        WorldSnapshot at14;
        ASSERT_TRUE(game.rollback.Save(14, at14));
        ASSERT_EQ(game.Capture(14), straight14);
        game.Run(14, 20);
        ASSERT_EQ(game.Capture(20), straight20);
        ASSERT_TRUE(game.rollback.Restore(at14).has_value());
        EXPECT_EQ(game.Capture(14), straight14);
        game.Run(14, 20);
        EXPECT_EQ(game.Capture(20), straight20);
    }

    TEST(RollbackPhysicsTest, AParkedBodyIsOutOfTheSimulationAndRestoreBringsItBack)
    {
        Game game;
        game.Run(0, 4);
        WorldSnapshot snapshot;
        ASSERT_TRUE(game.rollback.Save(4, snapshot));
        Rigidbody *const body = game.world.Find(NetworkId{2})->GetGameObject().GetComponent<Rigidbody>();
        BodyState state;
        ASSERT_TRUE(game.backend->GetBodyState(body->GetHandle(), state));

        game.Run(4, 10); // ball 2 is despawned at tick 8
        EXPECT_EQ(game.world.Find(NetworkId{2}), nullptr);
        EXPECT_FALSE(game.backend->GetBodyState(body->GetHandle(), state)) << "parked: disabled, not destroyed";
        EXPECT_TRUE(body->GetHandle().IsValid());

        ASSERT_TRUE(game.rollback.Restore(snapshot).has_value());
        EXPECT_TRUE(game.backend->GetBodyState(body->GetHandle(), state));
        EXPECT_NE(game.world.Find(NetworkId{2}), nullptr);
    }

    TEST(RollbackPhysicsTest, ReleasingTheParkedBodyMakesTheSnapshotUnrestorableWithoutChangingAnything)
    {
        Game game;
        game.Run(0, 4);
        WorldSnapshot snapshot;
        ASSERT_TRUE(game.rollback.Save(4, snapshot));
        game.Run(4, 10);
        EXPECT_GE(game.rollback.ReleaseConfirmed(9), 1u);
        game.scene->ProcessDestroyed();
        const Outcome before = game.Capture(10);

        const auto restored = game.rollback.Restore(snapshot);
        EXPECT_FALSE(restored.has_value());
        EXPECT_EQ(game.Capture(10), before);
    }

    TEST(RollbackPhysicsTest, APrefabWithARigidbodyOnAChildIsPooledAndKeepsItsBodyAcrossARollback)
    {
        Outcome straight10;
        Outcome straight20;
        {
            Game game;
            game.spawnPrefab = kPairPrefab;
            game.Run(0, 10);
            straight10 = game.Capture(10);
            game.Run(10, 20);
            straight20 = game.Capture(20);
        }
        ASSERT_TRUE(straight20.physics.has_value());

        Game game;
        game.spawnPrefab = kPairPrefab;
        game.Run(0, 4);
        WorldSnapshot at4;
        ASSERT_TRUE(game.rollback.Save(4, at4));
        game.Run(4, 10);
        ASSERT_EQ(game.Capture(10), straight10);
        NetworkObject *const pair = game.world.Find(NetworkId{3});
        ASSERT_NE(pair, nullptr);
        ASSERT_EQ(pair->GetPrefabName(), kPairPrefab);
        const Rigidbody *const childBody = pair->GetGameObject().GetChild(0)->GetComponent<Rigidbody>();
        ASSERT_NE(childBody, nullptr);
        const PhysicsBodyHandle handle = childBody->GetHandle();

        ASSERT_TRUE(game.rollback.Restore(at4).has_value());
        EXPECT_EQ(game.rollback.Stats().released, 0u) << "the spawned pair is parked for the pool, not destroyed";
        game.Run(4, 10);
        EXPECT_GE(game.rollback.Stats().spawnsPooled, 1u) << "the resimulated spawn took the parked pair back";
        NetworkObject *const again = game.world.Find(NetworkId{3});
        ASSERT_EQ(again, pair) << "the same object";
        EXPECT_EQ(again->GetGameObject().GetChild(0)->GetComponent<Rigidbody>()->GetHandle(), handle)
            << "the child's body kept its handle";
        // Same handles, so even HashState (handle order) is the straight run's
        EXPECT_EQ(game.Capture(10), straight10);
        game.Run(10, 20);
        EXPECT_EQ(game.Capture(20), straight20);
    }

    // Physics through the relay (C5 follow-up): a RollbackRuntime with a Jolt backend joins a LockstepRelay next to a peer
    // whose input changes every tick, so the guesses are wrong often and the Jolt world is rolled back many times, with
    // spawns, a despawn and impulses inside the window. The roots it publishes for confirmed checkpoints (the tree has the
    // physics leaf) must be those of a straight, never rolled back run of the same confirmed inputs.
    TEST(RollbackPhysicsTest, ARollbackRuntimeWithJoltThroughTheRelayPublishesTheStraightRunsRoots)
    {
        using namespace N2Engine::N2Net;
        auto created = InputLayout::Create({{"move", InputControlKind::Vector2, 8, 1.0f, true}});
        ASSERT_TRUE(created.has_value());
        const InputLayout layout = *created;
        constexpr std::uint32_t kInterval = 10;
        const auto logic = [&layout](Game &game, RollbackWorld &rb, const TickInputs &inputs)
        {
            game.Logic(inputs.tick, rb);
            const auto values = layout.Unpack(inputs.inputs[1].Bytes());
            if (values && std::abs((*values)[0]) > 0.5f)
            {
                game.world.Find(NetworkId{1})->GetGameObject().GetComponent<Rigidbody>()->AddImpulse(
                    Vector3((*values)[0] * 0.2f, 0.0f, (*values)[1] * 0.2f));
            }
            return true;
        };

        SimNetwork network(91);
        SimTransport &relayTransport = network.AddEndpoint();
        SimTransport &transportA = network.AddEndpoint();
        SimTransport &transportB = network.AddEndpoint();
        SimLinkConfig link;
        link.latencyMicros = 25'000;
        link.jitterMicros = 15'000;
        ASSERT_TRUE(network.Link(relayTransport.Id(), transportA.Id(), link));
        ASSERT_TRUE(network.Link(relayTransport.Id(), transportB.Id(), link));
        LockstepRelayOptions relayOptions;
        relayOptions.protocolId = 0x6363;
        relayOptions.slotCount = 2;
        relayOptions.layout = layout;
        relayOptions.tickRate = 60;
        relayOptions.hashInterval = kInterval;
        LockstepRelay relay(relayTransport, relayOptions);

        Application &app = Application::GetInstance();
        std::vector<TickInputs> confirmed;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> published;
        std::uint64_t rollbacks = 0;
        {
            Game game;
            NetworkSettings settings;
            settings.preset = NetworkPreset::Deterministic;
            settings.deterministic.mode = DeterministicMode::Rollback;
            settings.deterministic.rollbackWindow = 8;
            RollbackWorld *runtimeWorld = nullptr;
            RollbackRuntimeOptions options;
            options.session.protocolId = 0x6363;
            options.session.layout = layout;
            options.session.historyDepth = 64;
            options.session.sampleInput = [](const std::uint32_t) { return std::vector<float>{0.0f, 0.0f}; };
            options.session.applyTick = [&](const TickInputs &inputs, std::uint32_t)
            { return logic(game, *runtimeWorld, inputs); };
            options.clock = [&network] { return network.NowMicros(); };
            RollbackRuntime runtime(transportA, game.world, game.backend, settings, std::move(options));
            runtimeWorld = &runtime.World();
            ASSERT_EQ(runtime.RollbackWindow(), 8u) << "the Jolt backend can snapshot, so the session is rollback";

            LockstepRuntimeOptions other;
            other.protocolId = 0x6363;
            other.layout = layout;
            other.sampleInput = [](const std::uint32_t tick)
            {
                return std::vector<float>{static_cast<float>((tick * 5) % 7) / 3.0f - 1.0f,
                                          static_cast<float>((tick * 3) % 5) / 2.0f - 1.0f};
            };
            other.applyTick = [&confirmed](const TickInputs &inputs, std::uint32_t)
            {
                confirmed.push_back(inputs);
                return true;
            };
            LockstepRuntime peerB(transportB, std::move(other));

            constexpr double kFrame = 1.0 / 60.0;
            for (int frame = 0; frame < 6000 && runtime.Session().ConfirmedTick() < 160; ++frame)
            {
                network.Advance(16'667);
                relay.Update(network.NowMicros());
                TickOptions tick;
                tick.pollEvents = false;
                tick.render = false;
                tick.deltaSeconds = kFrame;
                app.Tick(tick);
                peerB.Update(network.NowMicros(), kFrame);
            }
            ASSERT_EQ(runtime.Session().State(), LockstepState::Running) << runtime.Session().Error();
            ASSERT_GE(runtime.Session().ConfirmedTick(), 160u);
            ASSERT_NE(runtime.Session().Session(), nullptr);
            rollbacks = runtime.Session().Session()->Stats().rollbacks;
            for (const HashTree &tree : runtime.Session().History().Trees())
            {
                published.emplace_back(tree.Tick(), tree.Root());
            }
            EXPECT_EQ(relay.Stats().desyncs, 0u);
        }
        EXPECT_GT(rollbacks, 5u) << "the Jolt world was rolled back repeatedly";
        ASSERT_GE(published.size(), 8u);

        std::map<std::uint32_t, std::uint64_t> straight;
        {
            Game game;
            Scene &scene = SceneManager::GetCurSceneRef();
            for (const TickInputs &inputs : confirmed)
            {
                app.GetSimulationClock().SetTick(inputs.tick);
                logic(game, game.rollback, inputs);
                game.rollback.CommitSpawns(); // as RollbackRuntime::RunTick, before the step
                ASSERT_EQ(app.StepTicks(1), 1u);
                scene.ProcessAttachQueue();
                game.rollback.CommitSpawns();
                scene.ProcessDestroyed();
                if ((inputs.tick + 1) % kInterval == 0)
                {
                    straight[inputs.tick + 1] =
                        BuildStateHashTree(game.world, game.rollback.Layouts(), inputs.tick + 1, game.backend)->Root();
                }
            }
        }
        std::size_t compared = 0;
        for (const auto &[tick, root] : published)
        {
            if (const auto found = straight.find(tick); found != straight.end())
            {
                EXPECT_EQ(found->second, root) << "tick " << tick;
                ++compared;
            }
        }
        EXPECT_GE(compared, 8u);
        app.SetSimulationMode(SimulationMode::None);
    }

    TEST(RollbackPhysicsTest, ASerialisedSnapshotWithJoltBytesRestoresAJoinerExactlyAndBothRunOnAlike)
    {
        std::vector<std::uint8_t> bytes;
        Outcome first10;
        Outcome first14;
        {
            Game game;
            game.Run(0, 10); // ball 2 despawned at tick 8 (parked), the ball of tick 5 is id 3
            WorldSnapshot snapshot;
            ASSERT_TRUE(game.rollback.Save(10, snapshot));
            ASSERT_TRUE(snapshot.hasPhysics);
            bytes = SerializeWorldSnapshot(snapshot);
            first10 = game.Capture(10);
            game.Run(10, 14); // another spawn at tick 12
            first14 = game.Capture(14);
        }

        Game joiner; // the scene as loaded: balls 1 and 2, nothing spawned
        const auto decoded = DeserializeWorldSnapshot(bytes);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        const auto joined = joiner.rollback.RestoreForJoin(*decoded);
        ASSERT_TRUE(joined.has_value()) << joined.error();
        EXPECT_EQ(joined->spawned, 1u);
        EXPECT_EQ(joined->removed, 1u) << "ball 2 is gone on the other peer";
        EXPECT_TRUE(joined->exactPhysics) << "the joiner's bodies are the snapshot's: the Jolt bytes were taken";
        const Outcome at10 = joiner.Capture(10);
        EXPECT_EQ(at10.root, first10.root);
        EXPECT_EQ(at10.fields, first10.fields);
        EXPECT_EQ(at10.simulated, first10.simulated) << "the session hash agrees (the other peer still parks ball 2)";
        joiner.Run(10, 14);
        const Outcome at14 = joiner.Capture(14);
        EXPECT_EQ(at14.root, first14.root);
        EXPECT_EQ(at14.fields, first14.fields);
        EXPECT_EQ(at14.simulated, first14.simulated);
    }
}

#endif // N2ENGINE_JOLT_ENABLED
