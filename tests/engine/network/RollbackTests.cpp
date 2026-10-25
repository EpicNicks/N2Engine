#include <gtest/gtest.h>

#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <math/Random.hpp>

#include "NetworkTestSupport.hpp"
#include "RollbackTestSupport.hpp"
#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/RollbackEvents.hpp"
#include "engine/network/RollbackSession.hpp"
#include "engine/network/RollbackWorld.hpp"

// Netcode C5: the rollback world (snapshots, parked spawns and despawns), rollback-aware events, and the rollback session
// (prediction, misprediction, resimulation, the bounded window), each against the lockstep result for the same inputs.
// The scripted-latency session test is in RollbackSessionNetworkTests.cpp; the physics part in
// tests/engine/physics/RollbackPhysicsTests.cpp.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using namespace N2Engine::Network::Testing::Rollback;

namespace
{
    constexpr std::uint32_t kDelay = 2;

    N2Net::TickInputs InputsFor(const InputLayout &layout, const std::uint32_t slots, const std::uint32_t tick)
    {
        N2Net::TickInputs inputs;
        inputs.tick = tick;
        for (std::uint32_t slot = 0; slot < slots; ++slot)
        {
            inputs.inputs[slot] = tick < kDelay ? N2Net::LockstepInput::FromBytes(layout.NeutralPayload()).value()
                                                : ScriptedInput(layout, slot, tick);
        }
        return inputs;
    }

    /// The reference: a plain loop, no rollback, no snapshots (what a lockstep session runs)
    struct Reference
    {
        Arena arena;
        RollbackEventQueue queue{RollbackEventPolicy::OnConfirm};
        std::vector<RollbackEvent> played;

        Reference(const std::uint32_t slots, const InputLayout &layout) : arena(slots, layout)
        {
            arena.events = &queue;
            queue.SetPlayHandler([this](const RollbackEvent &event) { played.push_back(event); });
        }

        void Run(const std::uint32_t ticks)
        {
            for (std::uint32_t tick = 0; tick < ticks; ++tick)
            {
                queue.BeginTick(tick);
                ASSERT_TRUE(arena.Step(InputsFor(arena.layout, arena.slots, tick)));
                arena.rollback.ReleaseConfirmed(tick + 1);
                queue.Confirm(tick + 1);
            }
        }
    };

    /// One rollback peer: the arena and the session driving it
    struct Peer
    {
        Arena arena;
        std::unique_ptr<RollbackSession> session;
        std::vector<RollbackEvent> played;
        std::vector<RollbackEvent> cancelled;

        Peer(const std::uint32_t slots, const std::uint32_t localSlot, const std::uint32_t window, const InputLayout &layout,
             const RollbackEventPolicy policy = RollbackEventPolicy::Immediate)
            : arena(slots, layout)
        {
            RollbackConfig config;
            config.slotCount = slots;
            config.localSlot = localSlot;
            config.inputDelay = kDelay;
            config.rollbackWindow = window;
            config.eventPolicy = policy;
            config.layout = *MakeLockstepLayoutInfo(layout);
            RollbackHost host;
            host.save = [this](const std::uint32_t tick, WorldSnapshot &out) { return arena.rollback.Save(tick, out); };
            host.restore = [this](const WorldSnapshot &snapshot) { return arena.rollback.Restore(snapshot); };
            host.step = [this](const N2Net::TickInputs &inputs) { return arena.Step(inputs); };
            host.confirmed = [this](const std::uint32_t first) { arena.rollback.ReleaseConfirmed(first); };
            host.resimulated = [this] { arena.rollback.ReleasePool(); };
            session = std::make_unique<RollbackSession>(config, std::move(host));
            arena.events = &session->Events();
            session->Events().SetPlayHandler([this](const RollbackEvent &event) { played.push_back(event); });
            session->Events().SetCancelHandler([this](const RollbackEvent &event) { cancelled.push_back(event); });
        }

        /// Submits the scripted local input for the next local tick
        void SampleLocal()
        {
            std::uint32_t assigned = 0;
            ASSERT_EQ(session->SubmitLocal(ScriptedInput(arena.layout, session->Config().localSlot, session->Config().inputDelay + localSampled), assigned),
                      N2Net::SubmitResult::Accepted);
            ASSERT_EQ(assigned, session->Config().inputDelay + localSampled);
            ++localSampled;
        }

        /// A remote slot's scripted input for `tick`
        N2Net::SubmitResult Deliver(const std::uint32_t slot, const std::uint32_t tick)
        {
            return session->SubmitRemote(slot, tick, ScriptedInput(arena.layout, slot, tick));
        }

        std::uint32_t localSampled = 0;
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// NetworkWorld parking
// ---------------------------------------------------------------------------------------------------------------------

TEST(NetworkWorldParkingTest, ParkHidesTheObjectAndUnparkRestoresIt)
{
    std::unique_ptr<Scene> scene = MakeScene("park", {10, 20});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    GameObject::Ptr second = FindByNumber(*scene, 20);
    second->SetActive(false); // was inactive before it was parked: it must come back inactive

    ASSERT_TRUE(world.Park(NetworkId{2}, NetworkWorld::ParkReason::Despawned, 7));
    EXPECT_EQ(world.Find(NetworkId{2}), nullptr);
    EXPECT_EQ(world.Count(), 1u);
    ASSERT_NE(world.FindParked(NetworkId{2}), nullptr);
    EXPECT_EQ(world.FindParked(NetworkId{2})->tick, 7u);
    EXPECT_FALSE(world.FindParked(NetworkId{2})->wasActive);
    EXPECT_FALSE(second->IsActive());
    EXPECT_FALSE(world.Park(NetworkId{2}, NetworkWorld::ParkReason::Despawned, 8)) << "already parked";
    EXPECT_FALSE(world.Park(NetworkId{99}, NetworkWorld::ParkReason::Despawned, 8)) << "unknown";

    NetworkObject *const back = world.Unpark(NetworkId{2});
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(world.Find(NetworkId{2}), back);
    EXPECT_EQ(back->GetNetworkId().value, 2u);
    EXPECT_FALSE(second->IsActive());
    EXPECT_EQ(world.Unpark(NetworkId{2}), nullptr);
}

TEST(NetworkWorldParkingTest, AParkedObjectKeepsItsIdAndTheIdIsNotReusable)
{
    ScopedPrefab prefab("park_prefab");
    std::unique_ptr<Scene> scene = MakeScene("park2", {10});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    const NetworkId id = world.AllocateId();
    ASSERT_TRUE(world.SpawnInstance(id, prefab.Name(), 3).has_value());
    ASSERT_TRUE(world.Park(id, NetworkWorld::ParkReason::SpawnUndone, 1));
    EXPECT_FALSE(world.SpawnInstance(id, prefab.Name(), 3).has_value()) << "a parked id is in use";
    EXPECT_EQ(world.FindParked(id)->object->GetOwner(), 3u);
    EXPECT_TRUE(world.FindParked(id)->object->IsSpawned());
}

TEST(NetworkWorldParkingTest, ReleaseDestroysTheObjectAndResetReactivatesTheRest)
{
    ScopedPrefab prefab("park_release");
    std::unique_ptr<Scene> scene = MakeScene("park3", {10, 20});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    const NetworkId spawned = world.AllocateId();
    NetworkObject *const object = world.SpawnInstance(spawned, prefab.Name(), 0).value();
    const GameObject::Ptr spawnedObject = object->GetGameObject().shared_from_this();

    ASSERT_TRUE(world.Park(spawned, NetworkWorld::ParkReason::Despawned, 1));
    ASSERT_TRUE(world.Park(NetworkId{1}, NetworkWorld::ParkReason::Despawned, 1));
    EXPECT_TRUE(world.ReleaseParked(spawned));
    EXPECT_FALSE(world.ReleaseParked(spawned));
    scene->ProcessDestroyed();
    EXPECT_TRUE(spawnedObject->IsDestroyed());
    EXPECT_EQ(world.Parked().size(), 1u);

    world.Reset();
    EXPECT_TRUE(world.Parked().empty());
    EXPECT_TRUE(FindByNumber(*scene, 10)->IsActive()) << "a world that is reset gives back what it parked";
}

TEST(NetworkWorldParkingTest, AParkedObjectDestroyedByTheSceneLeavesTheParkedSet)
{
    std::unique_ptr<Scene> scene = MakeScene("park4", {10, 20});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    ASSERT_TRUE(world.Park(NetworkId{1}, NetworkWorld::ParkReason::Despawned, 1));
    FindByNumber(*scene, 10)->Destroy();
    scene->ProcessDestroyed();
    EXPECT_TRUE(world.Parked().empty());
    EXPECT_EQ(world.Count(), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rollback events
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    struct EventLog
    {
        std::vector<RollbackEventKey> played;
        std::vector<RollbackEventKey> cancelled;

        void Attach(RollbackEventQueue &queue)
        {
            queue.SetPlayHandler([this](const RollbackEvent &event) { played.push_back(event.key); });
            queue.SetCancelHandler([this](const RollbackEvent &event) { cancelled.push_back(event.key); });
        }
    };

    void EmitAt(RollbackEventQueue &queue, const std::uint32_t tick, const std::uint32_t netId, const std::uint32_t eventId,
                const std::vector<std::uint8_t> &payload = {})
    {
        queue.BeginTick(tick);
        queue.Emit(netId, eventId, payload);
    }
}

TEST(RollbackEventsTest, ImmediateEventsPlayOnceAndASameKeyResimulationDoesNotReplayThem)
{
    RollbackEventQueue queue(RollbackEventPolicy::Immediate);
    EventLog log;
    log.Attach(queue);
    EmitAt(queue, 5, 3, 1);
    EmitAt(queue, 6, 3, 2, {9});
    ASSERT_EQ(log.played.size(), 2u);

    queue.BeginResimulation(5);
    EmitAt(queue, 5, 3, 1);
    EmitAt(queue, 6, 3, 2, {9});
    queue.EndResimulation();
    EXPECT_EQ(log.played.size(), 2u) << "the same effects were recognised, not played again";
    EXPECT_TRUE(log.cancelled.empty());
    EXPECT_EQ(queue.Stats().matched, 2u);
    EXPECT_EQ(queue.Pending(), 2u);
}

TEST(RollbackEventsTest, AnEffectTheResimulationDoesNotProduceIsCancelled)
{
    RollbackEventQueue queue(RollbackEventPolicy::Immediate);
    EventLog log;
    log.Attach(queue);
    EmitAt(queue, 5, 3, 1);
    EmitAt(queue, 6, 3, 2);
    EmitAt(queue, 7, 4, 2);

    queue.BeginResimulation(6);
    EmitAt(queue, 6, 3, 2); // kept
    queue.BeginTick(7);     // tick 7's effect does not happen any more
    queue.EndResimulation();

    ASSERT_EQ(log.cancelled.size(), 1u);
    EXPECT_EQ(log.cancelled[0].tick, 7u);
    EXPECT_EQ(log.cancelled[0].netId, 4u);
    EXPECT_EQ(queue.Pending(), 2u) << "tick 5 was before the rollback and stays";
}

TEST(RollbackEventsTest, ANewEffectOfTheResimulationPlaysAndAChangedPayloadReplaces)
{
    RollbackEventQueue queue(RollbackEventPolicy::Immediate);
    EventLog log;
    log.Attach(queue);
    EmitAt(queue, 6, 3, 2, {1});

    queue.BeginResimulation(6);
    EmitAt(queue, 6, 3, 2, {2}); // same key, other contents: the old one is taken back, this one is new
    queue.Emit(8, 5);            // a new effect
    queue.EndResimulation();

    ASSERT_EQ(log.cancelled.size(), 1u);
    EXPECT_EQ(log.played.size(), 3u);
    EXPECT_EQ(log.played[1].tick, 6u);
    EXPECT_EQ(log.played[2].netId, 8u);
}

TEST(RollbackEventsTest, IdenticalEmissionsInOneTickAreToldApartByOccurrence)
{
    RollbackEventQueue queue(RollbackEventPolicy::Immediate);
    EventLog log;
    log.Attach(queue);
    queue.BeginTick(4);
    queue.Emit(1, 1);
    queue.Emit(1, 1);
    ASSERT_EQ(log.played.size(), 2u);
    EXPECT_EQ(log.played[0].occurrence, 0u);
    EXPECT_EQ(log.played[1].occurrence, 1u);

    queue.BeginResimulation(4);
    queue.BeginTick(4);
    queue.Emit(1, 1); // only one of the two happens in the resimulation
    queue.EndResimulation();
    ASSERT_EQ(log.cancelled.size(), 1u);
    EXPECT_EQ(log.cancelled[0].occurrence, 1u);
}

TEST(RollbackEventsTest, ConfirmFinalisesEventsSoALaterRollbackCannotCancelThem)
{
    RollbackEventQueue queue(RollbackEventPolicy::Immediate);
    EventLog log;
    log.Attach(queue);
    EmitAt(queue, 3, 1, 1);
    EmitAt(queue, 9, 1, 2);
    queue.Confirm(5);
    EXPECT_EQ(queue.Pending(), 1u);

    queue.BeginResimulation(9);
    queue.BeginTick(9);
    queue.EndResimulation();
    EXPECT_EQ(log.cancelled.size(), 1u) << "only the unconfirmed one";
    EXPECT_EQ(log.cancelled[0].tick, 9u);
}

TEST(RollbackEventsTest, OnConfirmPlaysNothingUntilTheTickIsFinalAndNeverCancels)
{
    RollbackEventQueue queue(RollbackEventPolicy::OnConfirm);
    EventLog log;
    log.Attach(queue);
    EmitAt(queue, 5, 2, 1);
    EmitAt(queue, 6, 2, 2);
    EmitAt(queue, 7, 2, 3);
    EXPECT_TRUE(log.played.empty());

    queue.BeginResimulation(6);
    EmitAt(queue, 6, 2, 2);
    queue.BeginTick(7); // tick 7's effect is gone
    queue.EndResimulation();
    EXPECT_TRUE(log.cancelled.empty());
    EXPECT_EQ(queue.Stats().dropped, 1u);

    queue.Confirm(7);
    ASSERT_EQ(log.played.size(), 2u);
    EXPECT_EQ(log.played[0].tick, 5u);
    EXPECT_EQ(log.played[1].tick, 6u);
    EXPECT_EQ(queue.Pending(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// RollbackWorld
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    /// Runs ticks [from, to) of the scripted game on `arena`
    void RunTicks(Arena &arena, RollbackEventQueue &queue, const std::uint32_t from, const std::uint32_t to)
    {
        for (std::uint32_t tick = from; tick < to; ++tick)
        {
            queue.BeginTick(tick);
            ASSERT_TRUE(arena.Step(InputsFor(arena.layout, arena.slots, tick)));
        }
    }

    struct WorldRig
    {
        ShotPrefab prefab;
        Arena arena;
        RollbackEventQueue queue{RollbackEventPolicy::OnConfirm};

        WorldRig() : arena(2, MakeLayout()) { arena.events = &queue; }
    };
}

TEST(RollbackWorldTest, RestoreReturnsTheExactStateAndTheResimulationReachesTheSameFuture)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 12);
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(12, snapshot));
    const std::string dumpAt12 = rig.arena.Dump();
    const std::uint64_t rootAt12 = rig.arena.Root(12);

    RunTicks(rig.arena, rig.queue, 12, 30);
    const std::string dumpAt30 = rig.arena.Dump();
    const std::uint64_t rootAt30 = rig.arena.Root(30);
    ASSERT_NE(dumpAt30, dumpAt12);

    ASSERT_TRUE(rig.arena.rollback.Restore(snapshot).has_value());
    EXPECT_EQ(rig.arena.Dump(), dumpAt12) << "every replicated field, unquantised, and the random stream";
    EXPECT_EQ(rig.arena.Root(12), rootAt12);

    RunTicks(rig.arena, rig.queue, 12, 30);
    EXPECT_EQ(rig.arena.Dump(), dumpAt30);
    EXPECT_EQ(rig.arena.Root(30), rootAt30);
}

TEST(RollbackWorldTest, ASpawnAfterTheRestorePointIsParkedAndTheResimulationReusesIt)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 15);
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(15, snapshot));
    const std::size_t liveAt15 = rig.arena.world.Count();
    const std::uint32_t nextIdAt15 = rig.arena.world.NextId();

    RunTicks(rig.arena, rig.queue, 15, 19); // the avatar of slot 1 fires at tick 16
    ASSERT_GT(rig.arena.world.Count(), liveAt15) << "a shot exists";
    const std::string dumpAt19 = rig.arena.Dump();

    ASSERT_TRUE(rig.arena.rollback.Restore(snapshot).has_value());
    EXPECT_EQ(rig.arena.world.Count(), liveAt15);
    EXPECT_EQ(rig.arena.world.NextId(), nextIdAt15);
    EXPECT_FALSE(rig.arena.world.Parked().empty());
    EXPECT_GE(rig.arena.rollback.Stats().spawnsUndone, 1u);

    RunTicks(rig.arena, rig.queue, 15, 19);
    EXPECT_GE(rig.arena.rollback.Stats().spawnsPooled, 1u) << "the same id and prefab took the parked object back";
    EXPECT_TRUE(rig.arena.world.Parked().empty());
    EXPECT_EQ(rig.arena.Dump(), dumpAt19) << "a pooled object starts from its freshly instantiated fields";
}

TEST(RollbackWorldTest, ADespawnAfterTheRestorePointIsUndone)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 17); // the shot of tick 16 is alive
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(17, snapshot));
    const std::string dumpAt17 = rig.arena.Dump();
    const std::size_t liveAt17 = rig.arena.world.Count();

    RunTicks(rig.arena, rig.queue, 17, 27); // it runs out and is despawned
    EXPECT_EQ(rig.arena.world.Find(NetworkId{3}), nullptr) << "the first shot is gone";
    EXPECT_FALSE(rig.arena.world.Parked().empty());

    ASSERT_TRUE(rig.arena.rollback.Restore(snapshot).has_value());
    EXPECT_EQ(rig.arena.world.Count(), liveAt17);
    EXPECT_EQ(rig.arena.Dump(), dumpAt17);
    EXPECT_GE(rig.arena.rollback.Stats().despawnsUndone, 1u);
}

TEST(RollbackWorldTest, ReleaseConfirmedDestroysDespawnedObjectsOnlyOnceNoSnapshotNeedsThem)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 27);
    ASSERT_FALSE(rig.arena.world.Parked().empty());
    const std::uint32_t parkedTick = rig.arena.world.Parked().begin()->second.tick;
    const std::size_t parked = rig.arena.world.Parked().size();

    EXPECT_EQ(rig.arena.rollback.ReleaseConfirmed(parkedTick), 0u) << "a snapshot at the despawn tick still needs it";
    EXPECT_EQ(rig.arena.world.Parked().size(), parked);
    EXPECT_GE(rig.arena.rollback.ReleaseConfirmed(parkedTick + 1), 1u);
}

TEST(RollbackWorldTest, RestoreRefusesAndChangesNothingWhenAnObjectWasReleased)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 17);
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(17, snapshot));
    RunTicks(rig.arena, rig.queue, 17, 27);
    rig.arena.rollback.ReleaseConfirmed(27);
    rig.arena.scene->ProcessDestroyed();
    const std::string before = rig.arena.Dump();

    const auto restored = rig.arena.rollback.Restore(snapshot);
    ASSERT_FALSE(restored.has_value());
    EXPECT_NE(restored.error().find("neither registered nor parked"), std::string::npos) << restored.error();
    EXPECT_EQ(rig.arena.Dump(), before);
}

TEST(RollbackWorldTest, NonReplicatedFieldsAreLeftAloneAndActiveFlagsAreRestored)
{
    WorldRig rig;
    RunTicks(rig.arena, rig.queue, 0, 4);
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(4, snapshot));

    GameObject &avatar = rig.arena.world.Find(NetworkId{1})->GetGameObject();
    avatar.GetComponent<Avatar>()->presentationOnly = 5;
    avatar.GetComponent<Avatar>()->x = 999.0f;
    avatar.SetActive(false);

    ASSERT_TRUE(rig.arena.rollback.Restore(snapshot).has_value());
    EXPECT_EQ(avatar.GetComponent<Avatar>()->presentationOnly, 5) << "presentation state is not sim state";
    EXPECT_NE(avatar.GetComponent<Avatar>()->x, 999.0f);
    EXPECT_TRUE(avatar.IsActive());
}

TEST(RollbackWorldTest, TheGlobalSeededRandomStreamIsRestored)
{
    struct Unseed
    {
        ~Unseed() { Math::Random::Unseed(); }
    } unseed;
    Math::Random::Seed(99);
    WorldRig rig;
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(0, snapshot));
    ASSERT_TRUE(snapshot.rng.has_value());
    const int first = Math::Random::RandomInt(0, 100000);
    const int second = Math::Random::RandomInt(0, 100000);
    ASSERT_TRUE(rig.arena.rollback.Restore(snapshot).has_value());
    EXPECT_EQ(Math::Random::RandomInt(0, 100000), first);
    EXPECT_EQ(Math::Random::RandomInt(0, 100000), second);
}

TEST(RollbackWorldTest, AnUnseededWorldSnapshotsNoGlobalStream)
{
    Math::Random::Unseed();
    WorldRig rig;
    WorldSnapshot snapshot;
    ASSERT_TRUE(rig.arena.rollback.Save(0, snapshot));
    EXPECT_FALSE(snapshot.rng.has_value());
}

// ---------------------------------------------------------------------------------------------------------------------
// RollbackSession
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    constexpr std::uint32_t kTicks = 40;

    std::vector<RollbackEventKey> LiveKeys(const std::vector<RollbackEvent> &played, const std::vector<RollbackEvent> &cancelled)
    {
        std::vector<RollbackEventKey> keys;
        for (const RollbackEvent &event : played)
        {
            keys.push_back(event.key);
        }
        for (const RollbackEvent &event : cancelled)
        {
            const auto found = std::find(keys.begin(), keys.end(), event.key);
            if (found != keys.end())
            {
                keys.erase(found);
            }
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    std::vector<RollbackEventKey> Keys(const std::vector<RollbackEvent> &played)
    {
        return LiveKeys(played, {});
    }

    /// Advances until nothing more can run
    AdvanceReport Drain(Peer &peer, std::uint32_t stopAt = 0xFFFFFFFFu)
    {
        AdvanceReport last;
        while (peer.session->CurrentTick() < stopAt)
        {
            last = peer.session->Advance();
            if (last.result != AdvanceResult::Ran)
            {
                break;
            }
        }
        return last;
    }
}

TEST(RollbackSessionTest, ALateDifferingInputRollsBackAndTheResultEqualsLockstep)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Reference reference(2, layout);
    reference.Run(kTicks);

    Peer peer(2, 0, 8, layout);
    for (std::uint32_t i = 0; i < kTicks - kDelay; ++i)
    {
        peer.SampleLocal();
    }
    // Nothing from slot 1 yet: the session runs on a guess until the window is full
    AdvanceReport report = Drain(peer);
    EXPECT_EQ(report.result, AdvanceResult::WindowFull);
    EXPECT_EQ(peer.session->CurrentTick() - peer.session->ConfirmedTick(), 8u);
    EXPECT_EQ(peer.session->ConfirmedTick(), kDelay);
    EXPECT_GT(peer.session->Stats().ticksPredicted, 0u);

    // The inputs arrive, and some of them are not what was guessed
    for (std::uint32_t tick = kDelay; tick < 10; ++tick)
    {
        EXPECT_EQ(peer.Deliver(1, tick), N2Net::SubmitResult::Accepted);
    }
    ASSERT_TRUE(peer.session->PendingRollback().has_value()) << "the scripted input differs from 'repeat the last one'";
    EXPECT_GT(peer.session->Stats().mispredictions, 0u);

    report = peer.session->Advance();
    EXPECT_TRUE(report.rolledBack);
    EXPECT_GT(report.resimulated, 0u);
    EXPECT_LE(report.resimulated, 8u) << "a rollback never resimulates more than the window";
    EXPECT_FALSE(peer.session->PendingRollback().has_value());

    // The rest of the match, delivered in order and just in time
    for (std::uint32_t tick = 10; tick < kTicks; ++tick)
    {
        EXPECT_EQ(peer.Deliver(1, tick), N2Net::SubmitResult::Accepted);
        Drain(peer);
    }
    Drain(peer);
    ASSERT_EQ(peer.session->CurrentTick(), kTicks);
    EXPECT_EQ(peer.session->ConfirmedTick(), kTicks);
    EXPECT_TRUE(peer.session->Error().empty()) << peer.session->Error();
    EXPECT_LE(peer.session->Stats().deepestRollback, 8u);

    EXPECT_EQ(peer.arena.Root(kTicks), reference.arena.Root(kTicks));
    EXPECT_EQ(peer.arena.Dump(), reference.arena.Dump());
    EXPECT_EQ(LiveKeys(peer.played, peer.cancelled), Keys(reference.played)) << "the effects that survived are the lockstep ones";
}

TEST(RollbackSessionTest, AnInputEqualToTheGuessCostsNothing)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Peer peer(2, 0, 8, layout);
    const N2Net::LockstepInput neutral = MakeLockstepLayoutInfo(layout)->neutral;
    for (std::uint32_t i = 0; i < 20 - kDelay; ++i)
    {
        peer.SampleLocal();
    }
    Drain(peer);
    for (std::uint32_t tick = kDelay; tick < 20; ++tick)
    {
        EXPECT_EQ(peer.session->SubmitRemote(1, tick, neutral), N2Net::SubmitResult::Accepted);
    }
    Drain(peer);
    EXPECT_EQ(peer.session->Stats().rollbacks, 0u);
    EXPECT_EQ(peer.session->Stats().mispredictions, 0u);
    EXPECT_EQ(peer.session->CurrentTick(), 20u);
    EXPECT_EQ(peer.session->ConfirmedTick(), 20u);
}

TEST(RollbackSessionTest, TheWindowBoundsHowFarTheSessionRunsAheadOfTheConfirmedTick)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    for (const std::uint32_t window : {1u, 3u, 8u, 15u})
    {
        Peer peer(2, 0, window, layout);
        for (std::uint32_t i = 0; i < 60; ++i)
        {
            peer.SampleLocal();
        }
        const AdvanceReport report = Drain(peer);
        EXPECT_EQ(report.result, AdvanceResult::WindowFull) << window;
        EXPECT_EQ(peer.session->CurrentTick(), peer.session->ConfirmedTick() + window) << window;
        EXPECT_GT(peer.session->Stats().windowFullWaits, 0u);
        // The ring holds a snapshot for every unconfirmed tick, and only those are needed
        for (std::uint32_t tick = peer.session->ConfirmedTick(); tick < peer.session->CurrentTick(); ++tick)
        {
            EXPECT_NE(peer.session->SnapshotAt(tick), nullptr) << window << " " << tick;
        }
    }
}

TEST(RollbackSessionTest, WindowZeroIsLockstep)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Reference reference(2, layout);
    reference.Run(kTicks);

    Peer peer(2, 0, 0, layout);
    for (std::uint32_t i = 0; i < kTicks - kDelay; ++i)
    {
        peer.SampleLocal();
    }
    Drain(peer); // the two neutral delay ticks need no sampling
    EXPECT_EQ(peer.session->CurrentTick(), kDelay);
    EXPECT_EQ(peer.session->Advance().result, AdvanceResult::WaitingForInput) << "no remote input: wait, never guess";
    for (std::uint32_t tick = kDelay; tick < kTicks; ++tick)
    {
        EXPECT_EQ(peer.Deliver(1, tick), N2Net::SubmitResult::Accepted);
        Drain(peer);
    }
    EXPECT_EQ(peer.session->CurrentTick(), kTicks);
    EXPECT_EQ(peer.session->Stats().ticksPredicted, 0u);
    EXPECT_EQ(peer.session->Stats().snapshotsSaved, 0u);
    EXPECT_EQ(peer.session->Stats().rollbacks, 0u);
    EXPECT_EQ(peer.arena.Root(kTicks), reference.arena.Root(kTicks));
    EXPECT_EQ(peer.arena.Dump(), reference.arena.Dump());
}

TEST(RollbackSessionTest, InputsAreCheckedAndConflictsKeepTheFirst)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Peer peer(2, 0, 4, layout);
    const N2Net::LockstepInput a = ScriptedInput(layout, 1, 6);
    const N2Net::LockstepInput b = MakeInput(layout, 1.0f, 1.0f, true);
    ASSERT_FALSE(a == b);

    EXPECT_EQ(peer.session->SubmitRemote(0, 5, a), N2Net::SubmitResult::BadSlot) << "the local slot is not remote";
    EXPECT_EQ(peer.session->SubmitRemote(2, 5, a), N2Net::SubmitResult::BadSlot);
    N2Net::LockstepInput shortInput = a;
    shortInput.size = static_cast<std::uint8_t>(a.size - 1);
    EXPECT_EQ(peer.session->SubmitRemote(1, 5, shortInput), N2Net::SubmitResult::BadPayload);
    EXPECT_EQ(peer.session->SubmitRemote(1, kRollbackInputWindow + kDelay, a), N2Net::SubmitResult::TooFar);

    EXPECT_EQ(peer.session->SubmitRemote(1, 6, a), N2Net::SubmitResult::Accepted);
    EXPECT_EQ(peer.session->SubmitRemote(1, 6, a), N2Net::SubmitResult::Duplicate);
    EXPECT_EQ(peer.session->SubmitRemote(1, 6, b), N2Net::SubmitResult::Conflict);
    ASSERT_TRUE(peer.session->ConfirmedInput(1, 6).has_value());
    EXPECT_EQ(*peer.session->ConfirmedInput(1, 6), a);

    // Ticks that are confirmed on every slot and have run are final: later copies are TooOld
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        peer.SampleLocal();
    }
    for (std::uint32_t tick = kDelay; tick < 6; ++tick)
    {
        ASSERT_EQ(peer.Deliver(1, tick), N2Net::SubmitResult::Accepted);
    }
    Drain(peer);
    ASSERT_GE(peer.session->ConfirmedTick(), 4u);
    EXPECT_EQ(peer.session->SubmitRemote(1, 2, a), N2Net::SubmitResult::TooOld);
}

TEST(RollbackSessionTest, TheLocalSlotRefusesInputFarBeyondTheConfirmedTick)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Peer peer(2, 0, 4, layout);
    std::uint32_t assigned = 0;
    std::uint32_t accepted = 0;
    N2Net::SubmitResult last = N2Net::SubmitResult::Accepted;
    while (last == N2Net::SubmitResult::Accepted && accepted < 500)
    {
        last = peer.session->SubmitLocal(ScriptedInput(layout, 0, kDelay + accepted), assigned);
        if (last == N2Net::SubmitResult::Accepted)
        {
            ++accepted;
        }
    }
    EXPECT_EQ(last, N2Net::SubmitResult::TooFar);
    EXPECT_EQ(accepted, kRollbackInputWindow - kDelay);
}

TEST(RollbackSessionTest, AMissingLocalInputWaits)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Peer peer(2, 0, 4, layout);
    Drain(peer); // the two neutral delay ticks need no sampling
    EXPECT_EQ(peer.session->CurrentTick(), kDelay);
    EXPECT_EQ(peer.session->Advance().result, AdvanceResult::WaitingForInput) << "the local slot has not sampled tick 2";
}

TEST(RollbackSessionTest, InvalidConfigurationsAreRefused)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    RollbackHost host;
    host.step = [](const N2Net::TickInputs &) { return true; };
    host.save = [](std::uint32_t, WorldSnapshot &) { return true; };
    host.restore = [](const WorldSnapshot &) { return std::expected<void, std::string>{}; };

    RollbackConfig config;
    config.layout = *MakeLockstepLayoutInfo(layout);
    EXPECT_TRUE(RollbackSession(config, host).Valid());

    RollbackConfig bad = config;
    bad.rollbackWindow = kMaxRollbackWindow + 1;
    EXPECT_FALSE(RollbackSession(bad, host).Valid());
    bad = config;
    bad.localSlot = 2;
    EXPECT_FALSE(RollbackSession(bad, host).Valid());
    bad = config;
    bad.slotCount = 0;
    EXPECT_FALSE(RollbackSession(bad, host).Valid());
    bad = config;
    bad.inputDelay = N2Net::kMaxInputDelay + 1;
    EXPECT_FALSE(RollbackSession(bad, host).Valid());

    RollbackHost noSave = host;
    noSave.save = nullptr;
    EXPECT_FALSE(RollbackSession(config, noSave).Valid());
    config.rollbackWindow = 0;
    EXPECT_TRUE(RollbackSession(config, noSave).Valid()) << "lockstep mode needs no snapshots";

    RollbackSession broken(bad, host);
    EXPECT_EQ(broken.Advance().result, AdvanceResult::Failed);
}

TEST(RollbackSessionTest, AFailingRestoreEndsTheSession)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Peer peer(2, 0, 8, layout);
    // Replace the restore with one that fails: swap in a session over the same arena
    RollbackConfig config = peer.session->Config();
    RollbackHost host;
    host.save = [&peer](const std::uint32_t tick, WorldSnapshot &out) { return peer.arena.rollback.Save(tick, out); };
    host.restore = [](const WorldSnapshot &) { return std::expected<void, std::string>(std::unexpected("boom")); };
    host.step = [&peer](const N2Net::TickInputs &inputs) { return peer.arena.Step(inputs); };
    RollbackSession session(config, host);
    peer.arena.events = &session.Events();
    for (std::uint32_t i = 0; i < 12; ++i)
    {
        std::uint32_t assigned = 0;
        ASSERT_EQ(session.SubmitLocal(ScriptedInput(layout, 0, kDelay + i), assigned), N2Net::SubmitResult::Accepted);
    }
    while (session.Advance().result == AdvanceResult::Ran)
    {
    }
    for (std::uint32_t tick = kDelay; tick < 6; ++tick)
    {
        (void)session.SubmitRemote(1, tick, ScriptedInput(layout, 1, tick));
    }
    ASSERT_TRUE(session.PendingRollback().has_value());
    EXPECT_EQ(session.Advance().result, AdvanceResult::Failed);
    EXPECT_NE(session.Error().find("boom"), std::string::npos) << session.Error();
    EXPECT_EQ(session.Advance().result, AdvanceResult::Failed);
}
