// The Jolt backend's snapshots (stage J6): SaveState, RestoreState and HashState. A snapshot round trip (save, step,
// restore, step again gives the same state hash, the same body states and the same events), rewinds with a corrected
// input, a touch that began before the snapshot and ends after the restore, what a restore puts back (mass, gravity,
// collider configuration), the refusals (a different set of bodies in the simulation, damaged bytes) that change
// nothing, parked bodies, the properties of HashState, and the same-binary 0-versus-N job-system thread count equality
// of the state (the proof the design asks of J6; docs/physics.html says exactly what it covers).

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;

        // A world built through the backend API alone (no components, so nothing but the backend's own state is in it):
        // a floor, three columns of boxes, a turned box, a capsule, a compound body, a kinematic platform that carries a
        // passenger, a ball that gets impulses, and a ring of spheres that fall and pile up.
        struct World
        {
            JoltBackend backend;
            std::vector<PhysicsBodyHandle> bodies; // every body created, in creation order
            PhysicsBodyHandle platform;
            PhysicsBodyHandle ball;

            explicit World(const bool deterministic = true, const int workers = JoltBackend::kWorkerThreads,
                           const int spheres = 10)
            {
                backend.SetDeterministic(deterministic);
                backend.SetWorkerThreadsForTesting(workers);
                EXPECT_TRUE(backend.Initialize());
                Build(spheres);
            }

            PhysicsBodyHandle Add(const PhysicsBodyHandle handle)
            {
                bodies.push_back(handle);
                return handle;
            }

            void Build(const int spheres)
            {
                const PhysicsMaterial material = PhysicsMaterial::Default();
                const Math::Quaternion identity = Math::Quaternion::Identity;
                const Math::Quaternion turned(0.92887f, 0.099f, 0.198f, 0.297f);

                const PhysicsBodyHandle floor = Add(backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), identity, nullptr));
                backend.AddBoxCollider(floor, nullptr, Vector3(30.0f, 0.5f, 30.0f), Vector3::Zero, material);

                for (int column = 0; column < 3; ++column)
                {
                    for (int level = 0; level < 4; ++level)
                    {
                        const float x = -4.0f + 2.5f * static_cast<float>(column) + 0.03f * static_cast<float>(level);
                        const float y = 0.5f + 1.02f * static_cast<float>(level);
                        const PhysicsBodyHandle box = Add(backend.CreateDynamicBody(Vector3(x, y, 0.0f), identity, 1.0f, nullptr, false));
                        backend.AddBoxCollider(box, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                    }
                }

                const PhysicsBodyHandle tilted = Add(backend.CreateDynamicBody(Vector3(3.0f, 2.0f, 0.0f), turned, 2.0f, nullptr, false));
                backend.AddBoxCollider(tilted, nullptr, Vector3(0.6f, 0.4f, 0.5f), Vector3::Zero, material);

                const PhysicsBodyHandle capsule = Add(backend.CreateDynamicBody(Vector3(6.0f, 3.0f, 0.0f), turned, 1.5f, nullptr, false));
                backend.AddCapsuleCollider(capsule, nullptr, 0.3f, 1.2f, Vector3::Zero, material);

                const PhysicsBodyHandle compound = Add(backend.CreateDynamicBody(Vector3(9.0f, 3.0f, 0.0f), identity, 3.0f, nullptr, false));
                backend.AddBoxCollider(compound, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                backend.AddSphereCollider(compound, nullptr, 0.5f, Vector3(1.5f, 0.0f, 0.0f), material);

                platform = Add(backend.CreateDynamicBody(Vector3(-8.0f, 1.0f, 4.0f), identity, 1.0f, nullptr, true));
                backend.AddBoxCollider(platform, nullptr, Vector3(2.0f, 0.2f, 2.0f), Vector3::Zero, material);
                const PhysicsBodyHandle passenger = Add(backend.CreateDynamicBody(Vector3(-8.0f, 2.0f, 4.0f), identity, 1.0f, nullptr, false));
                backend.AddSphereCollider(passenger, nullptr, 0.4f, Vector3::Zero, material);

                ball = Add(backend.CreateDynamicBody(Vector3(0.0f, 3.0f, 3.0f), identity, 1.0f, nullptr, false));
                backend.AddSphereCollider(ball, nullptr, 0.5f, Vector3::Zero, material);
                backend.SetVelocity(ball, Vector3(2.0f, 0.0f, 1.0f));

                for (int i = 0; i < spheres; ++i)
                {
                    const float x = -6.0f + 1.1f * static_cast<float>(i % 12);
                    const float y = 4.0f + 1.3f * static_cast<float>(i / 12) + 0.2f * static_cast<float>(i % 3);
                    const float z = -3.0f + 0.7f * static_cast<float>(i % 5);
                    const PhysicsBodyHandle sphere = Add(backend.CreateDynamicBody(Vector3(x, y, z), identity, 0.5f, nullptr, false));
                    backend.AddSphereCollider(sphere, nullptr, 0.4f, Vector3::Zero, material);
                }
            }
        };

        // The inputs of tick `tick` (1-based) for input `variant`: the kinematic platform's target every tick, an impulse on
        // the ball every 40th tick (its direction is the variant), and a gravity change (when it comes is the variant)
        void Input(World &world, const int tick, const int variant)
        {
            const float t = static_cast<float>(tick);
            world.backend.SetBodyTransform(world.platform, Vector3(-8.0f + 0.02f * t, 1.0f, 4.0f), Math::Quaternion::Identity);
            if (tick % 40 == 10)
            {
                world.backend.AddImpulse(world.ball, variant == 0 ? Vector3(1.5f, 0.0f, 0.0f) : Vector3(-1.0f, 0.5f, 0.7f));
            }
            if (tick == 80 && variant == 0)
            {
                world.backend.SetGravity(Vector3(0.5f, -9.81f, 0.0f));
            }
            if (tick == 100 && variant == 1)
            {
                world.backend.SetGravity(Vector3(-0.5f, -9.81f, 0.3f));
            }
        }

        // One tick the way Application::PhysicsUpdate runs it
        void Tick(World &world, const int tick, const int variant)
        {
            Input(world, tick, variant);
            world.backend.ApplyPendingChanges();
            world.backend.Update(kStep);
            world.backend.SyncTransforms();
            world.backend.ProcessCollisionCallbacks();
        }

        // Ticks `from` to `to`, both included
        void Run(World &world, const int from, const int to, const int variant)
        {
            for (int tick = from; tick <= to; ++tick)
            {
                Tick(world, tick, variant);
            }
        }

        std::uint64_t Hash(const JoltBackend &backend)
        {
            const std::optional<std::uint64_t> hash = backend.HashState();
            EXPECT_TRUE(hash.has_value());
            return hash.value_or(0);
        }

        std::vector<BodyState> States(const World &world)
        {
            std::vector<BodyState> states;
            for (const PhysicsBodyHandle handle : world.bodies)
            {
                BodyState state;
                world.backend.GetBodyState(handle, state); // a disabled body leaves the default state
                states.push_back(state);
            }
            return states;
        }

        bool SameState(const BodyState &a, const BodyState &b)
        {
            return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
                   a.rotation.GetX() == b.rotation.GetX() && a.rotation.GetY() == b.rotation.GetY() &&
                   a.rotation.GetZ() == b.rotation.GetZ() && a.rotation.GetW() == b.rotation.GetW() &&
                   a.linearVelocity.x == b.linearVelocity.x && a.linearVelocity.y == b.linearVelocity.y &&
                   a.linearVelocity.z == b.linearVelocity.z && a.angularVelocity.x == b.angularVelocity.x &&
                   a.angularVelocity.y == b.angularVelocity.y && a.angularVelocity.z == b.angularVelocity.z &&
                   a.sleeping == b.sleeping;
        }

        // Every state bit for bit equal (no tolerance: a restore and a replay must give the same numbers)
        bool SameStates(const std::vector<BodyState> &a, const std::vector<BodyState> &b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (!SameState(a[i], b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // The largest difference of any position coordinate between two lists of states
        float MaxPositionDifference(const std::vector<BodyState> &a, const std::vector<BodyState> &b)
        {
            if (a.size() != b.size())
            {
                return std::numeric_limits<float>::infinity();
            }
            float largest = 0.0f;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                largest = std::max(largest, std::abs(a[i].position.x - b[i].position.x));
                largest = std::max(largest, std::abs(a[i].position.y - b[i].position.y));
                largest = std::max(largest, std::abs(a[i].position.z - b[i].position.z));
            }
            return largest;
        }

        // The index of the first difference of two hash series, -1 when they are equal
        int FirstMismatch(const std::vector<std::uint64_t> &a, const std::vector<std::uint64_t> &b)
        {
            if (a.size() != b.size())
            {
                return 0;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (a[i] != b[i])
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        unsigned long long Printable(const std::uint64_t value)
        {
            return static_cast<unsigned long long>(value);
        }
    }

    // ===== round trip and rewinds =====

    TEST(JoltSnapshotTest, RestoringGivesTheSavedStateAndTheSameFuture)
    {
        World world;
        Run(world, 1, 60, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));
        EXPECT_FALSE(saved.empty());
        const std::uint64_t hashAtSave = Hash(world.backend);
        const std::vector<BodyState> statesAtSave = States(world);

        Run(world, 61, 120, 0);
        const std::uint64_t hashAfter = Hash(world.backend);
        const std::vector<BodyState> statesAfter = States(world);
        EXPECT_NE(hashAfter, hashAtSave) << "the scene did not move between the save and the restore";
        EXPECT_FALSE(SameStates(statesAfter, statesAtSave));

        ASSERT_TRUE(world.backend.RestoreState(saved));
        EXPECT_EQ(Hash(world.backend), hashAtSave);
        EXPECT_TRUE(SameStates(States(world), statesAtSave)) << "the restore did not bring every body back";

        Run(world, 61, 120, 0);
        EXPECT_EQ(Hash(world.backend), hashAfter) << "the second run of the same ticks differs from the first";
        EXPECT_TRUE(SameStates(States(world), statesAfter));
    }

    TEST(JoltSnapshotTest, AStateRestoredIntoAnotherBackendOfTheSameSceneGivesTheSameFuture)
    {
        // The scene is the same, the instance is not: what a rollback does after a reconnect or a second peer
        World first;
        Run(first, 1, 60, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(first.backend.SaveState(saved));
        Run(first, 61, 100, 0);

        World second;
        Run(second, 1, 20, 1); // another history, so the restore has something to undo
        ASSERT_TRUE(second.backend.RestoreState(saved));
        Run(second, 61, 100, 0);
        EXPECT_EQ(Hash(second.backend), Hash(first.backend));
        EXPECT_TRUE(SameStates(States(second), States(first)));
    }

    TEST(JoltSnapshotTest, RewindingKTicksWithACorrectedInputEqualsAStraightRunWithIt)
    {
        constexpr int kSaveTick = 60;
        constexpr int kLastTick = 150;

        // The straight run with the corrected input (variant 1) from the tick after the save; the hash of every tick
        World straight;
        Run(straight, 1, kSaveTick, 0);
        std::vector<std::uint64_t> straightHashes;
        for (int tick = kSaveTick + 1; tick <= kLastTick; ++tick)
        {
            Tick(straight, tick, 1);
            straightHashes.push_back(Hash(straight.backend));
        }

        for (const int k : {1, 5, 20, 40})
        {
            World rolled;
            Run(rolled, 1, kSaveTick, 0);
            std::vector<std::byte> saved;
            ASSERT_TRUE(rolled.backend.SaveState(saved));

            Run(rolled, kSaveTick + 1, kSaveTick + k, 0); // predicted with the wrong input
            ASSERT_TRUE(rolled.backend.RestoreState(saved)) << "k = " << k;

            std::vector<std::uint64_t> replayed;
            for (int tick = kSaveTick + 1; tick <= kLastTick; ++tick)
            {
                Tick(rolled, tick, 1);
                replayed.push_back(Hash(rolled.backend));
            }
            EXPECT_EQ(FirstMismatch(replayed, straightHashes), -1) << "rewinding " << k << " ticks";
            EXPECT_TRUE(SameStates(States(rolled), States(straight))) << "rewinding " << k << " ticks";
            EXPECT_EQ(rolled.backend.GetGravity().x, straight.backend.GetGravity().x) << "gravity was not rewound";
        }
    }

    TEST(JoltSnapshotTest, TheSavedAndTheNotYetAppliedGravityAreBothRestored)
    {
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        backend.SetGravity(Vector3(0.0f, -3.0f, 0.0f));
        backend.ApplyPendingChanges();
        backend.SetGravity(Vector3(1.0f, -4.0f, 2.0f)); // pending
        std::vector<std::byte> saved;
        ASSERT_TRUE(backend.SaveState(saved));

        backend.ApplyPendingChanges();
        backend.SetGravity(Vector3(9.0f, 9.0f, 9.0f));
        backend.ApplyPendingChanges();
        EXPECT_EQ(backend.GetGravity().y, 9.0f);

        ASSERT_TRUE(backend.RestoreState(saved));
        EXPECT_EQ(backend.GetGravity().y, -3.0f) << "the gravity in force at the save";
        backend.ApplyPendingChanges();
        EXPECT_EQ(backend.GetGravity().x, 1.0f) << "the change that was still pending at the save";
        EXPECT_EQ(backend.GetGravity().y, -4.0f);
        EXPECT_EQ(backend.GetGravity().z, 2.0f);
        backend.ApplyPendingChanges();
        EXPECT_EQ(backend.GetGravity().y, -4.0f) << "nothing is pending any more";
    }

    // ===== what a restore refuses, and that a refusal changes nothing =====

    TEST(JoltSnapshotTest, ARestoreRefusesABodyThatIsNewInTheSimulationAndChangesNothing)
    {
        World world;
        Run(world, 1, 40, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));

        // A body that spawned inside the window and stayed in the simulation
        const PhysicsBodyHandle extra =
            world.backend.CreateDynamicBody(Vector3(0.0f, 8.0f, -6.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
        world.backend.AddSphereCollider(extra, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
        Run(world, 41, 60, 0);

        const std::uint64_t before = Hash(world.backend);
        const std::vector<BodyState> statesBefore = States(world);
        EXPECT_FALSE(world.backend.RestoreState(saved));
        EXPECT_EQ(Hash(world.backend), before) << "a refused restore changed the state";
        EXPECT_TRUE(SameStates(States(world), statesBefore));
        BodyState extraState;
        EXPECT_TRUE(world.backend.GetBodyState(extra, extraState)) << "the new body is still simulated";

        // Parked (taken out of the simulation, handle kept), it no longer stands in the way
        world.backend.SetBodyEnabled(extra, false);
        EXPECT_TRUE(world.backend.RestoreState(saved));
    }

    TEST(JoltSnapshotTest, ARestoreRefusesADestroyedBodyAndABodyThatIsParkedNowButWasSimulated)
    {
        World world;
        Run(world, 1, 40, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));
        Run(world, 41, 50, 0);

        // A body that despawned inside the window and was parked: it is out of the simulation now, was in it at the save
        const PhysicsBodyHandle parked = world.bodies[3];
        world.backend.SetBodyEnabled(parked, false);
        const std::uint64_t before = Hash(world.backend);
        EXPECT_FALSE(world.backend.RestoreState(saved));
        EXPECT_EQ(Hash(world.backend), before);
        // Brought back before the restore, the state fits again
        world.backend.SetBodyEnabled(parked, true);
        EXPECT_TRUE(world.backend.RestoreState(saved));

        // A body destroyed inside the window cannot come back: the restore refuses for good
        world.backend.DestroyBody(world.bodies[4]);
        const std::uint64_t afterDestroy = Hash(world.backend);
        EXPECT_FALSE(world.backend.RestoreState(saved));
        EXPECT_EQ(Hash(world.backend), afterDestroy);
    }

    TEST(JoltSnapshotTest, ARestoreRefusesBytesThatAreNotASnapshotAndChangesNothing)
    {
        World world;
        Run(world, 1, 30, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));
        Run(world, 31, 50, 0);
        const std::uint64_t before = Hash(world.backend);
        const std::vector<BodyState> statesBefore = States(world);

        const auto expectRefused = [&](const std::span<const std::byte> bytes, const char *what)
        {
            EXPECT_FALSE(world.backend.RestoreState(bytes)) << what;
            EXPECT_EQ(Hash(world.backend), before) << what << ": the refused restore changed the state";
            EXPECT_TRUE(SameStates(States(world), statesBefore)) << what;
        };

        expectRefused(std::span<const std::byte>(), "empty");
        expectRefused(std::span<const std::byte>(saved.data(), 3), "three bytes");
        expectRefused(std::span<const std::byte>(saved.data(), saved.size() - 1), "one byte short");

        std::vector<std::byte> longer = saved;
        longer.push_back(std::byte{0});
        expectRefused(longer, "one byte too long");

        std::vector<std::byte> flipped = saved;
        flipped[flipped.size() / 2] ^= std::byte{0x10};
        expectRefused(flipped, "one flipped bit in the middle");

        std::vector<std::byte> badMagic = saved;
        badMagic[0] ^= std::byte{0xFF};
        expectRefused(badMagic, "another magic number");

        // The intact bytes still restore after all the refusals
        EXPECT_TRUE(world.backend.RestoreState(saved));
    }

    TEST(JoltSnapshotTest, ABackendThatIsNotInitializedRefusesAndLeavesTheOutputAlone)
    {
        JoltBackend backend;
        std::vector<std::byte> bytes{std::byte{7}, std::byte{8}};
        EXPECT_FALSE(backend.SaveState(bytes));
        ASSERT_EQ(bytes.size(), 2u) << "a refused save leaves the output alone";
        EXPECT_EQ(bytes[0], std::byte{7});
        EXPECT_FALSE(backend.RestoreState(bytes));
        EXPECT_FALSE(backend.HashState().has_value());

        ASSERT_TRUE(backend.Initialize());
        EXPECT_TRUE(backend.HashState().has_value());
        backend.Shutdown();
        EXPECT_FALSE(backend.HashState().has_value());
    }

    // ===== parked bodies =====

    TEST(JoltSnapshotTest, ParkedBodiesRestoreFineAndStayParked)
    {
        World control;
        Run(control, 1, 40, 0);
        Run(control, 41, 100, 0);

        World world;
        Run(world, 1, 40, 0);
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));

        // Two bodies spawn inside the window and are parked at once (created, then taken out of the simulation); a third
        // is parked later
        std::vector<PhysicsBodyHandle> parked;
        for (int i = 0; i < 2; ++i)
        {
            const PhysicsBodyHandle extra = world.backend.CreateDynamicBody(
                Vector3(1.0f + static_cast<float>(i), 6.0f, -5.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
            world.backend.AddBoxCollider(extra, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, PhysicsMaterial::Default());
            world.backend.SetBodyEnabled(extra, false);
            parked.push_back(extra);
        }
        Run(world, 41, 60, 0);
        const PhysicsBodyHandle late =
            world.backend.CreateStaticBody(Vector3(0.0f, 0.0f, 12.0f), Math::Quaternion::Identity, nullptr);
        world.backend.AddBoxCollider(late, nullptr, Vector3(1.0f, 1.0f, 1.0f), Vector3::Zero, PhysicsMaterial::Default());
        world.backend.SetBodyEnabled(late, false);
        parked.push_back(late);
        Run(world, 61, 80, 0);

        ASSERT_TRUE(world.backend.RestoreState(saved));
        for (const PhysicsBodyHandle handle : parked)
        {
            EXPECT_FALSE(world.backend.IsBodyEnabled(handle)) << "a restore does not bring a parked body back";
            EXPECT_NE(world.backend.GetJoltBodyId(handle), UINT32_MAX) << "the parked body keeps its handle and Jolt id";
        }

        // The bodies of the simulation replay as in a world that never had the parked ones (to a millimetre: the parked
        // bodies were added to and taken out of Jolt's broad phase and active list, which a control never did)
        Run(world, 41, 100, 0);
        EXPECT_LT(MaxPositionDifference(States(world), States(control)), 1e-3f);

        // A parked body can come back, and the simulation goes on
        world.backend.SetBodyEnabled(parked.front(), true);
        EXPECT_TRUE(world.backend.IsBodyEnabled(parked.front()));
        Run(world, 101, 130, 0);
        BodyState state;
        EXPECT_TRUE(world.backend.GetBodyState(parked.front(), state));
    }

    // ===== HashState =====

    TEST(JoltSnapshotTest, TheStateHashFollowsTheSimulationAndNothingElse)
    {
        World a;
        World b;
        EXPECT_EQ(Hash(a.backend), Hash(b.backend)) << "two identical worlds";
        Run(a, 1, 30, 0);
        EXPECT_NE(Hash(a.backend), Hash(b.backend)) << "one of them has stepped";
        Run(b, 1, 30, 0);
        EXPECT_EQ(Hash(a.backend), Hash(b.backend));
        a.backend.AddImpulse(a.ball, Vector3(0.0f, 0.0f, 3.0f)); // one input that only one of them got
        Run(a, 31, 31, 0);
        Run(b, 31, 31, 0);
        EXPECT_NE(Hash(a.backend), Hash(b.backend)) << "the impulse changed the state, so the hash";
        // Saving does not change it, and neither does the bookkeeping that is not body state
        World c;
        Run(c, 1, 30, 0);
        const std::uint64_t before = Hash(c.backend);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(c.backend.SaveState(bytes));
        EXPECT_EQ(Hash(c.backend), before);
    }

    TEST(JoltSnapshotTest, TheStateHashTreatsMinusZeroAsZero)
    {
        const auto hashWithVelocity = [](const Vector3 &velocity)
        {
            JoltBackend backend;
            EXPECT_TRUE(backend.Initialize());
            backend.SetGravity(Vector3::Zero);
            backend.ApplyPendingChanges();
            const PhysicsBodyHandle body =
                backend.CreateDynamicBody(Vector3(0.0f, 5.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
            backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
            backend.SetVelocity(body, velocity);
            backend.SetAngularVelocity(body, velocity);
            return Hash(backend);
        };
        EXPECT_EQ(hashWithVelocity(Vector3(0.0f, 0.0f, 0.0f)), hashWithVelocity(Vector3(-0.0f, -0.0f, -0.0f)));
        EXPECT_NE(hashWithVelocity(Vector3(0.0f, 0.0f, 0.0f)), hashWithVelocity(Vector3(0.0f, 0.0f, 1.0f)));
    }

    // ===== the thread count =====

    TEST(JoltSnapshotTest, TheStateDoesNotDependOnTheJobSystemsThreadCount)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        GTEST_SKIP() << "the web build has no thread pool: the single-threaded job system is the only one";
#else
        constexpr int kTicks = 150;
        const auto record = [](World &world)
        {
            std::vector<std::uint64_t> hashes;
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(world, tick, 0);
                hashes.push_back(Hash(world.backend));
            }
            return hashes;
        };

        // 0 worker threads: physics.deterministic gives the single-threaded job system
        World single(true, JoltBackend::kWorkerThreads, 40);
        ASSERT_TRUE(single.backend.UsesSingleThreadedJobSystem());
        EXPECT_EQ(single.backend.GetJobSystemConcurrency(), 1);
        const std::vector<std::uint64_t> expected = record(single);
        ASSERT_EQ(expected.size(), static_cast<std::size_t>(kTicks));
        EXPECT_NE(expected.front(), expected.back());
        EXPECT_EQ(single.backend.GetContactThreadCount(), 1u) << "the single-threaded job system runs on the calling thread";

        // N worker threads: the thread pool, with the worker count asked for, hash for hash and tick for tick
        for (const int workers : {1, 2, 4})
        {
            World pooled(false, workers, 40);
            ASSERT_FALSE(pooled.backend.UsesSingleThreadedJobSystem());
            EXPECT_EQ(pooled.backend.GetJobSystemConcurrency(), workers + 1) << workers << " workers";
            const std::vector<std::uint64_t> hashes = record(pooled);
            EXPECT_EQ(FirstMismatch(hashes, expected), -1) << workers << " workers differ from 0 at that tick (0-based)";
            std::printf("[JOLT-SNAPSHOT-THREADS] workers: %d, concurrency: %d, threads that ran the contact listener: %zu, "
                        "hash after %d ticks: 0x%016llx (single-threaded: 0x%016llx)\n",
                        workers, pooled.backend.GetJobSystemConcurrency(), pooled.backend.GetContactThreadCount(), kTicks,
                        Printable(hashes.back()), Printable(expected.back()));
            std::fflush(stdout);
            if (workers >= 2)
            {
                EXPECT_GE(pooled.backend.GetContactThreadCount(), 2u)
                    << "the contact listener only ever ran on one thread: the workers took no part in the steps";
            }
        }
#endif
    }

    TEST(JoltSnapshotTest, ARollbackUnderWorkerThreadsReachesTheSingleThreadedState)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        GTEST_SKIP() << "the web build has no thread pool";
#else
        World single(true, JoltBackend::kWorkerThreads, 40);
        Run(single, 1, 120, 1);

        World pooled(false, 4, 40);
        Run(pooled, 1, 60, 1);
        std::vector<std::byte> saved;
        ASSERT_TRUE(pooled.backend.SaveState(saved));
        Run(pooled, 61, 100, 0); // the wrong input
        ASSERT_TRUE(pooled.backend.RestoreState(saved));
        Run(pooled, 61, 120, 1);
        EXPECT_EQ(Hash(pooled.backend), Hash(single.backend));
        EXPECT_TRUE(SameStates(States(pooled), States(single)));
#endif
    }

    // ===== events and colliders, in a scene =====

    namespace
    {
        std::vector<std::string> *g_log = nullptr;
        int g_tick = 0;

        // Writes every event it gets, with the tick it came in, to the log in progress
        class SnapshotRecorder final : public Component
        {
        public:
            explicit SnapshotRecorder(GameObject &gameObject) : Component(gameObject) {}
            [[nodiscard]] std::string GetTypeName() const override { return "SnapshotRecorder"; }

            void OnCollisionEnter(const Collision &c) override { Log("CollisionEnter", c.otherGameObject); }
            void OnCollisionStay(const Collision &c) override { Log("CollisionStay", c.otherGameObject); }
            void OnCollisionExit(const Collision &c) override { Log("CollisionExit", c.otherGameObject); }
            void OnTriggerEnter(const Trigger t) override { Log("TriggerEnter", t.otherGameObject); }
            void OnTriggerStay(const Trigger t) override { Log("TriggerStay", t.otherGameObject); }
            void OnTriggerExit(const Trigger t) override { Log("TriggerExit", t.otherGameObject); }

        private:
            void Log(const char *kind, const GameObject *other) const
            {
                if (g_log)
                {
                    g_log->push_back(std::format("{} {} {} {}", g_tick, GetGameObject().GetName(), kind,
                                                 other ? other->GetName() : std::string("-")));
                }
            }
        };

        int Count(const std::vector<std::string> &log, const std::string &needle)
        {
            int count = 0;
            for (const std::string &line : log)
            {
                if (line.find(needle) != std::string::npos)
                {
                    ++count;
                }
            }
            return count;
        }
    }

    class JoltSnapshotSceneTest : public PhysicsCoreFixture<JoltTag>
    {
    protected:
        // A floor, a trigger zone around the ball's resting place, and a ball that settles on the floor; every object
        // records its events. Steps to tick 30, when the ball touches the floor and is inside the zone.
        void BuildScene()
        {
            g_log = &_log;
            g_tick = 0;
            const auto floor = SpawnFloor();
            floor->AddComponent<SnapshotRecorder>();
            const auto zone = Spawn("Zone", Vector3(0.0f, 1.5f, 0.0f));
            auto *zoneCollider = zone->AddComponent<BoxCollider>();
            zoneCollider->SetSize(Vector3(6.0f, 2.0f, 6.0f));
            zoneCollider->SetIsTrigger(true);
            zone->AddComponent<SnapshotRecorder>();
            _ball = SpawnBall("Ball", Vector3(0.0f, 1.2f, 0.0f));
            _ball->AddComponent<SnapshotRecorder>();
            for (g_tick = 1; g_tick <= 30; ++g_tick)
            {
                Step(1);
            }
            _handle = _ball->GetComponent<Rigidbody>()->GetHandle();
        }

        void TearDown() override
        {
            g_log = nullptr;
            PhysicsCoreFixture<JoltTag>::TearDown();
        }

        // Ticks `from` to `to`, with an upward impulse on the ball at tick 31 when `kick` is set
        void RunTicks(const int from, const int to, const bool kick)
        {
            for (g_tick = from; g_tick <= to; ++g_tick)
            {
                if (g_tick == 31 && kick)
                {
                    Native().AddImpulse(_handle, Vector3(0.0f, 8.0f, 0.0f));
                }
                Step(1);
            }
        }

        std::uint64_t StateHash() { return Hash(Native()); }

        std::vector<std::string> _log;
        GameObject::Ptr _ball;
        PhysicsBodyHandle _handle;
    };

    TEST_F(JoltSnapshotSceneTest, ATouchThatBeganBeforeTheSnapshotEndsAfterARestoreAsItDidInTheStraightRun)
    {
        BuildScene();
        ASSERT_TRUE(_handle.IsValid());
        std::vector<std::byte> saved;
        ASSERT_TRUE(Native().SaveState(saved));

        // The straight run: the ball is kicked off the floor at tick 31 (Exit), flies out of the zone (TriggerExit) and
        // comes down again (Enter, TriggerEnter)
        _log.clear();
        RunTicks(31, 150, true);
        const std::vector<std::string> straight = _log;
        const std::uint64_t straightHash = StateHash();
        EXPECT_GE(Count(straight, "Ball CollisionExit Floor"), 1) << "the touch that began before the snapshot never ended";
        EXPECT_GE(Count(straight, "Ball TriggerExit Zone"), 1);
        EXPECT_GE(Count(straight, "Ball CollisionEnter Floor"), 1) << "the ball never came down again";
        EXPECT_GE(Count(straight, "Ball TriggerEnter Zone"), 1);

        // Rolled back, with a stretch of another timeline in between (no kick: the touches carry on through Stay events)
        ASSERT_TRUE(Native().RestoreState(saved));
        Native().SyncTransforms();
        _log.clear();
        RunTicks(31, 50, false);
        EXPECT_EQ(Count(_log, "Exit"), 0) << "the wrong timeline had no Exit";
        EXPECT_GE(Count(_log, "Stay"), 1);
        ASSERT_TRUE(Native().RestoreState(saved));
        Native().SyncTransforms();
        _log.clear();
        RunTicks(31, 150, true);

        EXPECT_EQ(_log, straight) << "the replay delivered another sequence of events than the straight run";
        EXPECT_EQ(StateHash(), straightHash);
    }

    TEST_F(JoltSnapshotSceneTest, ColliderMassAndGravityChangesInsideTheWindowAreUndoneByARestore)
    {
        BuildScene();
        ASSERT_TRUE(_handle.IsValid());
        ICollider *collider = _ball->GetComponent<SphereCollider>();
        ASSERT_NE(collider, nullptr);
        const float massAtSave = Native().GetMass(_handle);
        const std::size_t shapesAtSave = Native().GetShapeCount(_handle);
        std::vector<std::byte> saved;
        ASSERT_TRUE(Native().SaveState(saved));

        _log.clear();
        RunTicks(31, 70, false);
        const std::vector<std::string> control = _log;
        const std::uint64_t controlHash = StateHash();

        // Inside the window: another size and material, mass, no gravity, a trigger, another layer
        ASSERT_TRUE(Native().RestoreState(saved));
        Native().UpdateSphereCollider(_handle, collider, 0.25f, Vector3::Zero, PhysicsMaterial::Rubber());
        Native().SetMass(_handle, 7.0f);
        Native().SetGravityEnabled(_handle, false);
        Native().SetIsTrigger(_handle, collider, true);
        Native().SetColliderLayer(_handle, collider, 5);
        RunTicks(31, 50, false);
        EXPECT_NE(StateHash(), controlHash);
        EXPECT_EQ(Native().GetShapeCount(_handle), 0u) << "the only collider is a trigger now";

        ASSERT_TRUE(Native().RestoreState(saved));
        EXPECT_EQ(Native().GetMass(_handle), massAtSave);
        EXPECT_EQ(Native().GetShapeCount(_handle), shapesAtSave);
        _log.clear();
        RunTicks(31, 70, false);
        EXPECT_EQ(StateHash(), controlHash) << "the configuration of the window was not undone";
        EXPECT_EQ(_log, control);
    }
}

#endif // N2ENGINE_JOLT_ENABLED
