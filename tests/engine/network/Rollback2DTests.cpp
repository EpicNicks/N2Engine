// Box2D (2D physics) under rollback (issue #381): the 2D world is in the rollback snapshot and in the state hash. With
// N2ENGINE_USE_BOX2D=OFF this file compiles to nothing (the build has no 2D world to roll back; the stripped tests of
// tests/engine/physics2d say what happens instead).

#ifdef N2ENGINE_BOX2D_ENABLED

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "RollbackTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/LockstepProtocol.hpp"
#include "engine/network/LockstepRelay.hpp"
#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/RollbackRuntime.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using namespace N2Engine::Network::Testing::Rollback;
using namespace N2Engine::N2Net;

namespace
{
    constexpr const char *kShot2DPrefab = "Rb2DShotPrefab";

    /// The prefab "Rb2DShotPrefab": a NetworkObject, a Shot and a dynamic 2D body with a circle, for this object's life
    class Shot2DPrefab
    {
    public:
        Shot2DPrefab()
        {
            RegisterTestComponents();
            const GameObject::Ptr root = GameObject::Create("Shot2DPrefab");
            root->CreatePositionable();
            root->AddComponent<NetworkObject>();
            root->AddComponent<Shot>();
            root->AddComponent<Physics2D::Rigidbody2D>();
            root->AddComponent<Physics2D::CircleCollider2D>()->SetRadius(0.2f);
            EXPECT_TRUE(PrefabManager::RegisterPrefab(kShot2DPrefab, root));
        }
        ~Shot2DPrefab() { PrefabManager::UnregisterPrefab(kShot2DPrefab); }
        Shot2DPrefab(const Shot2DPrefab &) = delete;
        Shot2DPrefab &operator=(const Shot2DPrefab &) = delete;
    };

    /// (fire, move x, move y) of `slot` at `tick`; a different `variant` is a different (wrong) guess of the same tick
    std::vector<float> Values2D(const std::uint32_t slot, const std::uint32_t tick, const std::uint32_t variant)
    {
        const std::uint32_t phase = (tick + slot * 3 + variant * 2) / 4;
        const float moveX = static_cast<float>((phase * 7 + slot * 3 + variant) % 9) / 4.0f - 1.0f;
        const float moveY = static_cast<float>((phase * 5 + slot + variant) % 7) / 3.0f - 1.0f;
        const bool fire = (phase + slot + variant) % 3 == 0 && (tick % 4) == 1;
        return {fire ? 1.0f : 0.0f, moveX, moveY};
    }

    /// The current scene: a static floor and two networked balls (net ids 1 and 2, each a body and a Rigidbody2D), so the
    /// scene has a 2D world from the start, with shots (a body each) spawned and despawned by the game's logic
    struct Game2D
    {
        Game2D()
        {
            RegisterTestComponents();
            SceneManager::AddScene(Scene::Create("Rb2DGame_" + std::to_string(++counter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Scene &scene = SceneManager::GetCurSceneRef();
            const GameObject::Ptr floor = GameObject::Create("Floor");
            floor->CreatePositionable();
            floor->GetPositionable()->SetPosition(Math::Vector3(0.0f, -1.0f, 0.0f));
            floor->AddComponent<Physics2D::BoxCollider2D>()->SetHalfExtents(Math::Vector2(30.0f, 1.0f));
            scene.AddRootGameObject(floor);
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                const GameObject::Ptr ball = GameObject::Create("Ball" + std::to_string(slot));
                ball->SetUUID(TestUuid(10 + slot * 10));
                ball->CreatePositionable();
                ball->GetPositionable()->SetPosition(Math::Vector3(slot == 0 ? -2.0f : 2.0f, 1.0f, 0.0f));
                ball->AddComponent<NetworkObject>();
                ball->AddComponent<Avatar>();
                ball->AddComponent<Physics2D::Rigidbody2D>();
                ball->AddComponent<Physics2D::CircleCollider2D>()->SetRadius(0.4f);
                scene.AddRootGameObject(ball);
            }
            EXPECT_TRUE(world.BindScene(scene).has_value());
            scene.ProcessAttachQueue();
        }

        ~Game2D()
        {
            SceneManager::AddScene(Scene::Create("Rb2DGame_Empty_" + std::to_string(++counter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
        }

        /// One tick of the game's logic: the balls are pushed by their input, fire spawns a shot with a body, shots fly (the
        /// engine moves their bodies) and run out
        static bool Logic(NetworkWorld &world, RollbackWorld &rollback, const std::uint32_t tick,
                          const std::array<std::vector<float>, 2> &values)
        {
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                NetworkObject *const object = world.Find(NetworkId{slot + 1});
                if (object == nullptr)
                {
                    return false;
                }
                GameObject &gameObject = object->GetGameObject();
                Avatar &avatar = *gameObject.GetComponent<Avatar>();
                Physics2D::Rigidbody2D *const body = gameObject.GetComponent<Physics2D::Rigidbody2D>();
                const std::vector<float> &input = values[slot];
                body->AddImpulse(Math::Vector2(input[1] * 0.3f, input[2] > 0.5f ? 0.8f : 0.0f));
                const Math::Vector3 position = gameObject.GetPositionable()->GetPosition();
                avatar.x = position.x;
                avatar.y = position.y;
                if (input[0] > 0.5f && static_cast<int>(tick) - avatar.lastFire >= 3)
                {
                    avatar.lastFire = static_cast<int>(tick);
                    auto spawned = rollback.Spawn(kShot2DPrefab, slot + 1, tick);
                    if (!spawned)
                    {
                        return false;
                    }
                    GameObject &shotObject = (*spawned)->GetGameObject();
                    Shot &shot = *shotObject.GetComponent<Shot>();
                    shot.x = position.x;
                    shot.life = 8 + static_cast<int>(tick % 4);
                    shot.shooter = static_cast<int>(slot);
                    shotObject.GetPositionable()->SetPosition(Math::Vector3(position.x, position.y + 0.9f, 0.0f));
                    shotObject.GetComponent<Physics2D::Rigidbody2D>()->SetLinearVelocity(
                        Math::Vector2(slot == 0 ? 3.0f : -3.0f, 4.0f));
                }
            }
            std::vector<std::uint32_t> shots;
            for (const auto &entry : world.Objects())
            {
                if (entry.second->IsSpawned())
                {
                    shots.push_back(entry.first);
                }
            }
            for (const std::uint32_t id : shots)
            {
                Shot &shot = *world.Find(NetworkId{id})->GetGameObject().GetComponent<Shot>();
                if (--shot.life <= 0)
                {
                    world.Find(NetworkId{static_cast<std::uint32_t>(shot.shooter) + 1})->GetGameObject().GetComponent<Avatar>()->score++;
                    (void)rollback.Despawn(NetworkId{id}, tick);
                }
            }
            return true;
        }

        static bool LogicFromInputs(NetworkWorld &world, RollbackWorld &rollback, const InputLayout &layout, const TickInputs &inputs)
        {
            std::array<std::vector<float>, 2> values;
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                const auto unpacked = layout.Unpack(inputs.inputs[slot].Bytes());
                if (!unpacked)
                {
                    return false;
                }
                values[slot] = *unpacked;
            }
            return Logic(world, rollback, inputs.tick, values);
        }

        NetworkWorld world;
        static inline int counter = 0;
    };

    /// One tick as the runtime runs it: the logic, the spawns committed, the step, the tick boundary
    bool RunTick(Game2D &game, RollbackWorld &rollback, const std::uint32_t tick, const std::uint32_t variant)
    {
        Application &app = Application::GetInstance();
        app.GetSimulationClock().SetTick(tick);
        const std::array<std::vector<float>, 2> values{Values2D(0, tick, variant), Values2D(1, tick, variant)};
        if (!Game2D::Logic(game.world, rollback, tick, values))
        {
            return false;
        }
        rollback.CommitSpawns();
        if (app.StepTicks(1) != 1)
        {
            return false;
        }
        Scene &scene = SceneManager::GetCurSceneRef();
        scene.ProcessAttachQueue();
        rollback.CommitSpawns();
        scene.ProcessDestroyed();
        return true;
    }

    std::uint64_t RootOf(Game2D &game, RollbackWorld &rollback, const std::uint32_t tick)
    {
        const auto tree = BuildStateHashTree(game.world, rollback.Layouts(), tick, nullptr);
        return tree ? tree->Root() : 0;
    }

    /// A tick as RunTick runs it, then the objects despawned three ticks ago released for good (their bodies destroyed, so
    /// Box2D gives their slots to later shots), the same way on every run of these ticks
    bool RunTickReleasing(Game2D &game, RollbackWorld &rollback, const std::uint32_t tick)
    {
        if (!RunTick(game, rollback, tick, 0))
        {
            return false;
        }
        if (tick >= 3)
        {
            (void)rollback.ReleaseConfirmed(tick - 3);
        }
        SceneManager::GetCurSceneRef().ProcessDestroyed();
        return true;
    }

    /// Box2D's deep hash of the current scene's 2D world: every body (parked ones too), the contacts and the id pools
    std::uint64_t Deep2DHash()
    {
        const Physics2D::Physics2DWorld *const world = SceneManager::GetCurSceneRef().GetPhysics2DWorld();
        return world != nullptr ? world->HashState() : 0;
    }

    void RestFrame()
    {
        TickOptions rest;
        rest.pollEvents = false;
        rest.render = false;
        rest.deltaSeconds = 0.0;
        Application::GetInstance().Tick(rest);
    }
}

TEST(Rollback2DTest, TheStateHashHasAPhysics2DLeafThatFollowsTheBodies)
{
    Shot2DPrefab prefab;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    Game2D game;
    RollbackWorld rollback(game.world, nullptr);
    ASSERT_NE(SceneManager::GetCurSceneRef().GetPhysics2DWorld(), nullptr);

    const auto first = BuildStateHashTree(game.world, rollback.Layouts(), 0, nullptr);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(DescribeHashedField(game.world, rollback.Layouts(), kPhysicsObjectId, kPhysics2DFieldId).empty());
    for (std::uint32_t tick = 0; tick < 10; ++tick)
    {
        ASSERT_TRUE(RunTick(game, rollback, tick, 0));
    }
    const auto later = BuildStateHashTree(game.world, rollback.Layouts(), 0, nullptr);
    ASSERT_TRUE(later.has_value());
    EXPECT_NE(first->Root(), later->Root());
}

TEST(Rollback2DTest, ARolledBackWorldWithShotsComingAndGoingRunsOnAsAStraightRunDoes)
{
    Shot2DPrefab prefab;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    constexpr std::uint32_t kTicks = 110;

    // The straight run: the right inputs, in order, no rollback
    std::map<std::uint32_t, std::uint64_t> straight;
    std::uint64_t spawnedInStraightRun = 0;
    {
        Game2D game;
        RollbackWorld rollback(game.world, nullptr);
        straight[0] = RootOf(game, rollback, 0);
        for (std::uint32_t tick = 0; tick < kTicks; ++tick)
        {
            ASSERT_TRUE(RunTick(game, rollback, tick, 0));
            straight[tick + 1] = RootOf(game, rollback, tick + 1);
            spawnedInStraightRun = game.world.NextId();
        }
    }
    ASSERT_GT(spawnedInStraightRun, 6u) << "the scene must have fired some shots";

    // The same ticks with wrong guesses run first from ticks 20, 45 and 70, then rewound and run right
    std::uint32_t rollbacks = 0;
    {
        Game2D game;
        RollbackWorld rollback(game.world, nullptr);
        for (std::uint32_t tick = 0; tick < kTicks; ++tick)
        {
            if (tick == 20 || tick == 45 || tick == 70)
            {
                WorldSnapshot snapshot;
                ASSERT_TRUE(rollback.Save(tick, snapshot));
                ASSERT_TRUE(snapshot.hasPhysics2d);
                ASSERT_FALSE(snapshot.physics2d.empty());
                const std::uint64_t guessedBefore = RootOf(game, rollback, tick);
                for (std::uint32_t ahead = 0; ahead < 6; ++ahead)
                {
                    ASSERT_TRUE(RunTick(game, rollback, tick + ahead, 1));
                }
                EXPECT_NE(RootOf(game, rollback, tick + 6), straight[tick + 6]) << "the wrong guess changed the world";
                const auto restored = rollback.Restore(snapshot);
                ASSERT_TRUE(restored.has_value()) << restored.error();
                ++rollbacks;
                EXPECT_EQ(RootOf(game, rollback, tick), guessedBefore) << "the restore put the world back, 2D leaf included";
                EXPECT_EQ(RootOf(game, rollback, tick), straight[tick]);
            }
            ASSERT_TRUE(RunTick(game, rollback, tick, 0));
            EXPECT_EQ(RootOf(game, rollback, tick + 1), straight[tick + 1]) << "tick " << tick;
        }
    }
    EXPECT_EQ(rollbacks, 3u);
    RestFrame();
}

TEST(Rollback2DTest, ASnapshotSavedWithoutAWorldCannotBeRestoredOntoOneAndTheErrorSaysWhy)
{
    Shot2DPrefab prefab;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    Game2D game;
    RollbackWorld rollback(game.world, nullptr);
    for (std::uint32_t tick = 0; tick < 5; ++tick)
    {
        ASSERT_TRUE(RunTick(game, rollback, tick, 0));
    }
    WorldSnapshot snapshot;
    ASSERT_TRUE(rollback.Save(5, snapshot));
    ASSERT_TRUE(snapshot.hasPhysics2d);

    WorldSnapshot without = snapshot;
    without.hasPhysics2d = false;
    without.physics2d.clear();
    const auto refused = rollback.Restore(without);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("2D"), std::string::npos) << refused.error();
    EXPECT_TRUE(rollback.Restore(snapshot).has_value());
}

// A late joiner (netcode C6) of a scene with a 2D world: the serialised snapshot (format 3) carries Box2D's state, each
// object's 2D bodies and the parked objects, so the joiner's 2D world is the other peer's, id pools and parked bodies
// included, even though the other peer made its shots' bodies in slots freed by earlier shots and the joiner makes them in
// net id order
TEST(Rollback2DJoinTest, ALateJoinerTakesTheSerialised2DStateExactlyAndRunsOnInStep)
{
    Shot2DPrefab prefab;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    constexpr std::uint32_t kAfter = 60;
    std::vector<std::uint8_t> bytes;
    std::uint32_t at = 0;
    std::size_t parkedThere = 0;
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> first; // tick: root, deep 2D hash
    {
        Game2D game;
        RollbackWorld rollback(game.world, nullptr);
        WorldSnapshot snapshot;
        // Run until shots are alive and some are parked (despawned, not released yet), after slots were freed and reused
        for (std::uint32_t tick = 0; tick < 400 && at == 0; ++tick)
        {
            ASSERT_TRUE(RunTickReleasing(game, rollback, tick));
            if (tick >= 40 && game.world.Count() > 2 && !game.world.Parked().empty())
            {
                at = tick + 1;
                ASSERT_TRUE(rollback.Save(at, snapshot));
            }
        }
        ASSERT_GT(at, 0u) << "a tick with live and parked shots";
        ASSERT_TRUE(snapshot.hasPhysics2d);
        ASSERT_FALSE(snapshot.parked.empty());
        parkedThere = snapshot.parked.size();
        bytes = SerializeWorldSnapshot(snapshot);
        first[at] = {RootOf(game, rollback, at), Deep2DHash()};
        for (std::uint32_t tick = at; tick < at + kAfter; ++tick)
        {
            ASSERT_TRUE(RunTickReleasing(game, rollback, tick));
            first[tick + 1] = {RootOf(game, rollback, tick + 1), Deep2DHash()};
        }
    }
    RestFrame();

    {
        Game2D joiner; // the scene as loaded: the floor and the two balls, no shots
        RollbackWorld rollback(joiner.world, nullptr);
        const auto decoded = DeserializeWorldSnapshot(bytes);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        ASSERT_TRUE(decoded->hasPhysics2d);
        EXPECT_EQ(decoded->parked.size(), parkedThere);
        const auto joined = rollback.RestoreForJoin(*decoded);
        ASSERT_TRUE(joined.has_value()) << joined.error();
        EXPECT_TRUE(joined->exactPhysics2d) << joined->lost2d << " lost, " << joined->removed2d << " removed";
        EXPECT_EQ(joined->lost2d, 0u);
        EXPECT_EQ(joined->removed2d, 0u);
        EXPECT_GT(joined->spawned, 0u) << "the live shots were instantiated under their ids";
        EXPECT_EQ(joined->parked, parkedThere) << "the parked shots were made and parked too";
        EXPECT_EQ(joiner.world.Parked().size(), parkedThere);
        EXPECT_EQ(RootOf(joiner, rollback, at), first[at].first);
        EXPECT_EQ(Deep2DHash(), first[at].second) << "Box2D's whole world, contacts and id pools, is the other peer's";
        for (std::uint32_t tick = at; tick < at + kAfter; ++tick)
        {
            ASSERT_TRUE(RunTickReleasing(joiner, rollback, tick));
            ASSERT_EQ(RootOf(joiner, rollback, tick + 1), first[tick + 1].first) << "tick " << tick;
            ASSERT_EQ(Deep2DHash(), first[tick + 1].second) << "tick " << tick;
        }
    }
    RestFrame();
}

// What a joiner whose 2D bodies do not match is told, and what a 2D part it cannot use does
TEST(Rollback2DJoinTest, AJoinThatCannotTakeThe2DPartExactlySaysSoAndTheSpectatorRuleApplies)
{
    Shot2DPrefab prefab;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    WorldSnapshot snapshot;
    {
        Game2D game;
        RollbackWorld rollback(game.world, nullptr);
        for (std::uint32_t tick = 0; tick < 30; ++tick)
        {
            ASSERT_TRUE(RunTickReleasing(game, rollback, tick));
        }
        ASSERT_TRUE(rollback.Save(30, snapshot));
    }
    RestFrame();

    // A Box2D image of another build (its header changed): refused before anything changes, the bodies are put back
    // from their saved poses
    {
        WorldSnapshot alien = snapshot;
        ASSERT_GT(alien.physics2d.size(), 12u + 16u);
        alien.physics2d[12 + 8] ^= std::byte{0x5A}; // the image's layout hash (after the wrapper's magic, version, size)
        Game2D joiner;
        RollbackWorld rollback(joiner.world, nullptr);
        const auto joined = rollback.RestoreForJoin(alien);
        ASSERT_TRUE(joined.has_value()) << joined.error();
        EXPECT_FALSE(joined->exactPhysics2d);
        const auto check = RollbackRuntime::CheckJoinPhysics(*joined, false, false, true);
        ASSERT_FALSE(check.has_value());
        EXPECT_NE(check.error().find("2D"), std::string::npos) << check.error();
        EXPECT_TRUE(RollbackRuntime::CheckJoinPhysics(*joined, false, true, true).has_value()) << "the host allowed it";
        EXPECT_TRUE(RollbackRuntime::CheckJoinPhysics(*joined, false, false, false).has_value()) << "no 2D part to refuse";
        const Physics2D::Rigidbody2D *const ball =
            joiner.world.Find(NetworkId{1})->GetGameObject().GetComponent<Physics2D::Rigidbody2D>();
        ASSERT_NE(ball, nullptr);
        EXPECT_TRUE(ball->GetHandle().IsValid()) << "the joiner's own body stays";
        const auto saved = std::ranges::find(snapshot.objects, 1u, &WorldSnapshot::Object::id);
        ASSERT_NE(saved, snapshot.objects.end());
        ASSERT_FALSE(saved->bodies2d.empty());
        const Math::Vector3 position = joiner.world.Find(NetworkId{1})->GetGameObject().GetPositionable()->GetPosition();
        EXPECT_FLOAT_EQ(position.x, saved->bodies2d[0].position.x) << "put back from the saved pose";
        EXPECT_FLOAT_EQ(position.y, saved->bodies2d[0].position.y);
        EXPECT_FLOAT_EQ(ball->GetLinearVelocity().x, saved->bodies2d[0].linearVelocityX);
    }
    RestFrame();

    // A snapshot without a 2D part into a scene with a 2D world is not this scene
    {
        WorldSnapshot without = snapshot;
        without.hasPhysics2d = false;
        without.physics2d.clear();
        Game2D joiner;
        RollbackWorld rollback(joiner.world, nullptr);
        const auto refused = rollback.RestoreForJoin(without);
        ASSERT_FALSE(refused.has_value());
        EXPECT_NE(refused.error().find("2D"), std::string::npos) << refused.error();
    }
    RestFrame();
}

TEST(Rollback2DApplicationTest, ARollbackRuntimeOverASceneWith2DPhysicsKeepsItsWindowRollsBackAndPublishesAStraightRunsRoots)
{
    Shot2DPrefab prefab;
    Application &app = Application::GetInstance();
    app.Set3DPhysicsBackend(nullptr);
    const InputLayout layout = MakeLayout();
    constexpr std::uint32_t kInterval = 10;

    SimNetwork network(47);
    SimTransport &relayTransport = network.AddEndpoint();
    SimTransport &transportA = network.AddEndpoint();
    SimTransport &transportB = network.AddEndpoint();
    SimTransport &transportC = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 25'000;
    link.jitterMicros = 15'000;
    ASSERT_TRUE(network.Link(relayTransport.Id(), transportA.Id(), link));
    ASSERT_TRUE(network.Link(relayTransport.Id(), transportB.Id(), link));
    ASSERT_TRUE(network.Link(relayTransport.Id(), transportC.Id(), link));
    LockstepRelayOptions relayOptions;
    relayOptions.protocolId = 0x6263;
    relayOptions.slotCount = 2;
    relayOptions.layout = layout;
    relayOptions.tickRate = 60;
    relayOptions.hashInterval = kInterval;
    LockstepRelay relay(relayTransport, relayOptions);

    std::vector<TickInputs> confirmed;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> published;
    std::uint64_t rollbacks = 0;
    // A spectator that joins once the session is under way: the rollback player (slot 0) serves it a snapshot of a scene
    // with a 2D world (format 3). It keeps the snapshot and the confirmed ticks it runs, so a fresh scene can play them
    // after the session (one scene steps at a time in this process)
    std::optional<WorldSnapshot> spectated;
    std::vector<TickInputs> spectatorTicks;
    std::unique_ptr<LockstepRuntime> spectator;
    {
        Game2D game;
        NetworkSettings settings;
        settings.preset = NetworkPreset::Deterministic;
        settings.deterministic.mode = DeterministicMode::Rollback;
        settings.deterministic.rollbackWindow = 8;

        RollbackWorld *rollbackWorld = nullptr;
        RollbackRuntimeOptions options;
        options.session.protocolId = 0x6263;
        options.session.layout = layout;
        options.session.historyDepth = 64;
        options.session.sampleInput = [](const std::uint32_t tick) { return Values2D(0, tick, 0); };
        options.session.applyTick = [&](const TickInputs &inputs, std::uint32_t)
        { return Game2D::LogicFromInputs(game.world, *rollbackWorld, layout, inputs); };
        options.clock = [&network] { return network.NowMicros(); };
        RollbackRuntime runtime(transportA, game.world, nullptr, settings, std::move(options));
        rollbackWorld = &runtime.World();
        EXPECT_EQ(runtime.RollbackWindow(), 8u) << "a 2D world no longer makes the session lockstep";

        LockstepRuntimeOptions other;
        other.protocolId = 0x6263;
        other.layout = layout;
        // Peer B's input changes every tick: a repeated guess is usually wrong
        other.sampleInput = [](const std::uint32_t tick) { return Values2D(1, tick * 3 + 1, 2); };
        other.applyTick = [&confirmed](const TickInputs &inputs, std::uint32_t)
        {
            confirmed.push_back(inputs);
            return true;
        };
        LockstepRuntime peerB(transportB, std::move(other));

        constexpr double kFrame = 1.0 / 60.0;
        for (int frame = 0; frame < 4000 && runtime.Session().ConfirmedTick() < 200; ++frame)
        {
            network.Advance(16'667);
            relay.Update(network.NowMicros());
            TickOptions tick;
            tick.pollEvents = false;
            tick.render = false;
            tick.deltaSeconds = kFrame;
            app.Tick(tick);
            peerB.Update(network.NowMicros(), kFrame);
            if (!spectator && runtime.Session().ConfirmedTick() >= 60)
            {
                LockstepRuntimeOptions watch;
                watch.protocolId = 0x6263;
                watch.layout = layout;
                watch.peerName = "spectator";
                watch.spectate = true;
                watch.join = [&spectated](const WorldSnapshot &snapshot) -> std::expected<void, std::string>
                {
                    spectated = snapshot;
                    return {};
                };
                watch.applyTick = [&spectatorTicks](const TickInputs &inputs, std::uint32_t)
                {
                    spectatorTicks.push_back(inputs);
                    return true;
                };
                spectator = std::make_unique<LockstepRuntime>(transportC, std::move(watch));
            }
            if (spectator)
            {
                spectator->Update(network.NowMicros(), kFrame);
            }
        }
        ASSERT_EQ(runtime.Session().State(), LockstepState::Running) << runtime.Session().Error();
        ASSERT_NE(spectator, nullptr);
        EXPECT_EQ(spectator->State(), LockstepState::Running) << spectator->Error();
        EXPECT_TRUE(spectator->IsSpectator());
        EXPECT_EQ(relay.Stats().spectatorsJoined, 1u);
        ASSERT_GE(runtime.Session().ConfirmedTick(), 200u);
        ASSERT_NE(runtime.Session().Session(), nullptr);
        rollbacks = runtime.Session().Session()->Stats().rollbacks;
        for (const HashTree &tree : runtime.Session().History().Trees())
        {
            published.emplace_back(tree.Tick(), tree.Root());
        }
        EXPECT_EQ(relay.Stats().desyncs, 0u);
    }
    RestFrame();
    EXPECT_GT(rollbacks, 0u) << "the guesses were wrong sometimes, so the 2D world was rewound";
    ASSERT_GE(published.size(), 10u);

    std::map<std::uint32_t, std::uint64_t> straight;
    {
        Game2D game;
        RollbackWorld rollbackWorld(game.world, nullptr);
        Scene &scene = SceneManager::GetCurSceneRef();
        for (const TickInputs &inputs : confirmed)
        {
            app.GetSimulationClock().SetTick(inputs.tick);
            ASSERT_TRUE(Game2D::LogicFromInputs(game.world, rollbackWorld, layout, inputs));
            rollbackWorld.CommitSpawns();
            ASSERT_EQ(app.StepTicks(1), 1u);
            scene.ProcessAttachQueue();
            rollbackWorld.CommitSpawns();
            scene.ProcessDestroyed();
            if ((inputs.tick + 1) % kInterval == 0)
            {
                straight[inputs.tick + 1] = RootOf(game, rollbackWorld, inputs.tick + 1);
            }
        }
    }
    std::size_t compared = 0;
    for (const auto &[tick, root] : published)
    {
        const auto found = straight.find(tick);
        if (found != straight.end())
        {
            EXPECT_EQ(found->second, root) << "tick " << tick;
            ++compared;
        }
    }
    EXPECT_GE(compared, 10u);

    // The spectator's view: a fresh scene takes the served snapshot (its 2D part exactly) and runs the confirmed ticks
    // the spectator ran; its checkpoint roots are the rollback player's
    ASSERT_TRUE(spectated.has_value()) << "the player answered the spectator's snapshot request";
    EXPECT_TRUE(spectated->hasPhysics2d);
    ASSERT_FALSE(spectatorTicks.empty());
    EXPECT_EQ(spectatorTicks.front().tick, spectated->tick) << "the spectator runs from the snapshot's tick";
    std::map<std::uint32_t, std::uint64_t> watched;
    {
        Game2D game;
        RollbackWorld rollbackWorld(game.world, nullptr);
        const auto joined = rollbackWorld.RestoreForJoin(*spectated);
        ASSERT_TRUE(joined.has_value()) << joined.error();
        EXPECT_TRUE(joined->exactPhysics2d) << joined->lost2d << " lost, " << joined->removed2d << " removed";
        EXPECT_TRUE(RollbackRuntime::CheckJoinPhysics(*joined, spectated->hasPhysics, false, spectated->hasPhysics2d).has_value());
        Scene &scene = SceneManager::GetCurSceneRef();
        for (const TickInputs &inputs : spectatorTicks)
        {
            app.GetSimulationClock().SetTick(inputs.tick);
            ASSERT_TRUE(Game2D::LogicFromInputs(game.world, rollbackWorld, layout, inputs));
            rollbackWorld.CommitSpawns();
            ASSERT_EQ(app.StepTicks(1), 1u);
            scene.ProcessAttachQueue();
            rollbackWorld.CommitSpawns();
            scene.ProcessDestroyed();
            if ((inputs.tick + 1) % kInterval == 0)
            {
                watched[inputs.tick + 1] = RootOf(game, rollbackWorld, inputs.tick + 1);
            }
        }
    }
    std::size_t watchedCompared = 0;
    for (const auto &[tick, root] : published)
    {
        const auto found = watched.find(tick);
        if (found != watched.end())
        {
            EXPECT_EQ(found->second, root) << "spectated tick " << tick;
            ++watchedCompared;
        }
    }
    EXPECT_GE(watchedCompared, 5u);
    app.SetSimulationMode(SimulationMode::None);
    RestFrame();
}

#endif
