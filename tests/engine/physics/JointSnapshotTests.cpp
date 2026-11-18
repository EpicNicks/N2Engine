// Joints in a Jolt snapshot (stage c of #381 item 6): SaveState writes every joint (its description, its constraint's
// settings and accumulated state, the order of Jolt's constraint list), RestoreState puts them back, and HashState and
// HashSimulatedState cover them. The proof the design asks for is a ragdoll scene (three ragdolls of twelve bodies, mixed
// Ball, Hinge, Slider, Fixed and Distance joints, one joint to the world, driven by an impulse input every few ticks):
// save at tick N, run on, restore, resume, and the state hash of every tick after N equals the straight run's, bit for
// bit; the same into a freshly created world; the same with the constraint list reordered or a limit changed inside the
// window; and a hash that changes when a joint's state differs. A change of the joint SET inside the window (a joint
// made or ended) is refused, which is the documented limit.

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "engine/Logger.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;
        constexpr int kSaveTick = 50;
        constexpr int kLastTick = 150;

        struct Ragdoll
        {
            std::vector<PhysicsBodyHandle> bodies;
            std::vector<PhysicsJointHandle> joints;
        };

        // Three ragdolls built through the backend API alone: a torso, a head (Ball), two shoulders (Ball), two elbows
        // (Hinge, limited), two hips (Ball), two knees (Slider, limited), two feet (Fixed), and a bob on a rope (Distance,
        // limited); the third one's head is also held to the world (Ball to the world). They fall onto a floor and onto
        // each other, so the constraints carry real impulses (and some come to rest, the bodies fall asleep).
        struct Scene
        {
            JoltBackend backend;
            std::vector<Ragdoll> ragdolls;
            std::vector<PhysicsBodyHandle> bodies; // every body, in creation order
            std::vector<PhysicsJointHandle> joints; // every joint, in creation order
            PhysicsJointHandle elbow;               // one with limits, for the tests that change them
            float displace = 0.0f;                  // the scene's bodies built this far off (a world that made the joints from other poses)

            explicit Scene(const bool deterministic = true, const int workers = JoltBackend::kWorkerThreads,
                           const float displaceBy = 0.0f)
                : displace(displaceBy)
            {
                backend.SetDeterministic(deterministic);
                backend.SetWorkerThreadsForTesting(workers);
                EXPECT_TRUE(backend.Initialize());
                Build();
            }

            PhysicsBodyHandle Body(Ragdoll &doll, const Vector3 &position, const Vector3 &half, const float mass,
                                   const bool sphere = false)
            {
                const PhysicsMaterial material = PhysicsMaterial::Default();
                const PhysicsBodyHandle handle = backend.CreateDynamicBody(position + Vector3(0.0f, displace, 0.0f),
                                                                           Math::Quaternion::Identity, mass, nullptr, false);
                if (sphere)
                {
                    backend.AddSphereCollider(handle, nullptr, half.x, Vector3::Zero, material);
                }
                else
                {
                    backend.AddBoxCollider(handle, nullptr, half, Vector3::Zero, material);
                }
                doll.bodies.push_back(handle);
                bodies.push_back(handle);
                return handle;
            }

            // A joint at the world point `at` between two bodies (their poses are as built: identity rotations)
            PhysicsJointHandle Join(Ragdoll &doll, const JointType type, const PhysicsBodyHandle a, const Vector3 &posA,
                                    const PhysicsBodyHandle b, const Vector3 &posB, const Vector3 &at,
                                    const Vector3 &axis = Vector3(0.0f, 0.0f, 1.0f), const bool limits = false,
                                    const float low = 0.0f, const float high = 0.0f)
            {
                JointDesc desc;
                desc.type = type;
                desc.bodyA = a;
                desc.bodyB = b;
                desc.anchorA = at - posA;
                desc.anchorB = at - posB;
                desc.axis = axis;
                desc.useLimits = limits;
                desc.limitMin = low;
                desc.limitMax = high;
                const PhysicsJointHandle joint = backend.CreateJoint(desc);
                EXPECT_TRUE(joint.IsValid());
                doll.joints.push_back(joint);
                joints.push_back(joint);
                return joint;
            }

            void Build()
            {
                const PhysicsMaterial material = PhysicsMaterial::Default();
                const PhysicsBodyHandle floor =
                    backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), Math::Quaternion::Identity, nullptr);
                backend.AddBoxCollider(floor, nullptr, Vector3(30.0f, 0.5f, 30.0f), Vector3::Zero, material);
                bodies.push_back(floor);

                for (int i = 0; i < 3; ++i)
                {
                    ragdolls.emplace_back();
                    Ragdoll &doll = ragdolls.back();
                    const float x = -3.0f + 3.0f * static_cast<float>(i);
                    const float y = 3.0f + 0.5f * static_cast<float>(i);
                    const Vector3 torsoPos(x, y, 0.0f);
                    const PhysicsBodyHandle torso = Body(doll, torsoPos, Vector3(0.3f, 0.5f, 0.15f), 4.0f);

                    const Vector3 headPos(x, y + 0.85f, 0.0f);
                    const PhysicsBodyHandle head = Body(doll, headPos, Vector3(0.22f, 0.0f, 0.0f), 1.0f, true);
                    Join(doll, JointType::Ball, torso, torsoPos, head, headPos, Vector3(x, y + 0.55f, 0.0f));

                    for (const float side : {-1.0f, 1.0f})
                    {
                        const Vector3 upperPos(x + side * 0.7f, y + 0.3f, 0.0f);
                        const PhysicsBodyHandle upper = Body(doll, upperPos, Vector3(0.25f, 0.1f, 0.1f), 1.0f);
                        Join(doll, JointType::Ball, torso, torsoPos, upper, upperPos, Vector3(x + side * 0.4f, y + 0.3f, 0.0f));
                        const Vector3 lowerPos(x + side * 1.2f, y + 0.3f, 0.0f);
                        const PhysicsBodyHandle lower = Body(doll, lowerPos, Vector3(0.25f, 0.08f, 0.08f), 0.8f);
                        const PhysicsJointHandle hinge =
                            Join(doll, JointType::Hinge, upper, upperPos, lower, lowerPos, Vector3(x + side * 0.95f, y + 0.3f, 0.0f),
                                 Vector3(0.0f, 0.0f, 1.0f), true, -1.2f, 0.3f);
                        if (i == 0 && side < 0.0f)
                        {
                            elbow = hinge;
                        }

                        const Vector3 thighPos(x + side * 0.2f, y - 0.75f, 0.0f);
                        const PhysicsBodyHandle thigh = Body(doll, thighPos, Vector3(0.12f, 0.3f, 0.12f), 2.0f);
                        Join(doll, JointType::Ball, torso, torsoPos, thigh, thighPos, Vector3(x + side * 0.2f, y - 0.5f, 0.0f));
                        const Vector3 shinPos(x + side * 0.2f, y - 1.4f, 0.0f);
                        const PhysicsBodyHandle shin = Body(doll, shinPos, Vector3(0.1f, 0.3f, 0.1f), 1.5f);
                        Join(doll, JointType::Slider, thigh, thighPos, shin, shinPos, Vector3(x + side * 0.2f, y - 1.05f, 0.0f),
                             Vector3(0.0f, 1.0f, 0.0f), true, -0.1f, 0.15f);
                        const Vector3 footPos(x + side * 0.2f, y - 1.75f, 0.1f);
                        const PhysicsBodyHandle foot = Body(doll, footPos, Vector3(0.12f, 0.05f, 0.2f), 0.6f);
                        Join(doll, JointType::Fixed, shin, shinPos, foot, footPos, Vector3(x + side * 0.2f, y - 1.7f, 0.0f));
                    }

                    const Vector3 bobPos(x, y - 0.5f, 0.9f);
                    const PhysicsBodyHandle bob = Body(doll, bobPos, Vector3(0.18f, 0.0f, 0.0f), 0.7f, true);
                    Join(doll, JointType::Distance, torso, torsoPos, bob, bobPos, Vector3(x, y - 0.2f, 0.15f), Vector3(0.0f, 0.0f, 1.0f),
                         true, 0.3f, 1.0f);

                    if (i == 2)
                    {
                        // The head held to a world point: body B is the world
                        JointDesc desc;
                        desc.type = JointType::Ball;
                        desc.bodyA = head;
                        desc.anchorA = Vector3(0.0f, 0.1f, 0.0f);
                        desc.anchorB = Vector3(x, y + 0.95f + displace, 0.0f);
                        const PhysicsJointHandle joint = backend.CreateJoint(desc);
                        EXPECT_TRUE(joint.IsValid());
                        doll.joints.push_back(joint);
                        joints.push_back(joint);
                    }
                }
            }
        };

        // The input of tick `tick` (1-based) for input `variant`: an impulse on one body of each ragdoll every 7th tick
        // (which body, and its direction, come from the tick, the ragdoll and the variant) and, at tick 90, a gravity
        // change that the variant decides
        void Input(Scene &scene, const int tick, const int variant)
        {
            for (std::size_t i = 0; i < scene.ragdolls.size(); ++i)
            {
                if (tick % 7 == static_cast<int>(i % 7))
                {
                    const Ragdoll &doll = scene.ragdolls[i];
                    const PhysicsBodyHandle target = doll.bodies[static_cast<std::size_t>(tick / 7 + variant) % doll.bodies.size()];
                    const float sign = ((tick / 7 + static_cast<int>(i)) % 2 == 0) ? 1.0f : -1.0f;
                    scene.backend.AddImpulse(target, Vector3(sign * (variant == 0 ? 1.2f : 0.7f), 0.8f, sign * 0.4f * static_cast<float>(variant + 1)));
                }
            }
            if (tick == 90 && variant == 1)
            {
                scene.backend.SetGravity(Vector3(0.4f, -9.81f, 0.2f));
            }
        }

        void Tick(Scene &scene, const int tick, const int variant)
        {
            Input(scene, tick, variant);
            scene.backend.ApplyPendingChanges();
            scene.backend.Update(kStep);
            scene.backend.SyncTransforms();
            scene.backend.ProcessCollisionCallbacks();
        }

        void RunRange(Scene &scene, const int from, const int to, const int variant)
        {
            for (int tick = from; tick <= to; ++tick)
            {
                Tick(scene, tick, variant);
            }
        }

        struct Hashes
        {
            std::uint64_t full = 0;
            std::uint64_t simulated = 0;
            bool operator==(const Hashes &) const = default;
        };

        Hashes HashOf(const JoltBackend &backend)
        {
            const std::optional<std::uint64_t> full = backend.HashState();
            const std::optional<std::uint64_t> simulated = backend.HashSimulatedState();
            EXPECT_TRUE(full.has_value());
            EXPECT_TRUE(simulated.has_value());
            return {full.value_or(0), simulated.value_or(0)};
        }

        // Ticks `from` to `to` with `variant`, the hashes after each
        std::vector<Hashes> Record(Scene &scene, const int from, const int to, const int variant)
        {
            std::vector<Hashes> hashes;
            for (int tick = from; tick <= to; ++tick)
            {
                Tick(scene, tick, variant);
                hashes.push_back(HashOf(scene.backend));
            }
            return hashes;
        }

        // The index of the first difference of two series, -1 when equal
        int FirstMismatch(const std::vector<Hashes> &a, const std::vector<Hashes> &b)
        {
            if (a.size() != b.size())
            {
                return 0;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (!(a[i] == b[i]))
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        std::vector<std::byte> Snapshot(const JoltBackend &backend)
        {
            std::vector<std::byte> bytes;
            EXPECT_TRUE(backend.SaveState(bytes));
            return bytes;
        }

        // How many bodies are awake: the ragdolls must still move at the save, or the test proves nothing
        int Awake(const Scene &scene)
        {
            int awake = 0;
            for (const PhysicsBodyHandle handle : scene.bodies)
            {
                BodyState state;
                if (scene.backend.GetBodyState(handle, state) && !state.sleeping)
                {
                    ++awake;
                }
            }
            return awake;
        }

        // Quiet the error log of the tests that expect a refusal
        class ErrorCapture
        {
        public:
            ErrorCapture()
            {
                _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
                {
                    if (level == Logger::LogLevel::Error)
                    {
                        errors.emplace_back(message);
                    }
                };
            }
            ~ErrorCapture() { Logger::logEvent -= _id; }
            ErrorCapture(const ErrorCapture &) = delete;
            ErrorCapture &operator=(const ErrorCapture &) = delete;
            std::vector<std::string> errors;

        private:
            std::size_t _id = 0;
        };
    }

    // ===== the ragdoll round trip =====

    TEST(JoltJointSnapshotTest, TheRagdollSceneHasEveryJointTypeAndMoves)
    {
        Scene scene;
        EXPECT_EQ(scene.joints.size(), 3u * 12u + 1u);
        RunRange(scene, 1, kSaveTick, 0);
        EXPECT_GT(Awake(scene), 10) << "the ragdolls are at rest at the save: nothing about the constraints is tested";
        std::size_t joints = 0;
        for (const PhysicsJointHandle joint : scene.joints)
        {
            joints += scene.backend.IsJointValid(joint) ? 1 : 0;
        }
        EXPECT_EQ(joints, scene.joints.size());
    }

    TEST(JoltJointSnapshotTest, SaveRunOnRestoreAndResumeEqualsTheStraightRunTickForTick)
    {
        // Every pool setting a rollback test uses: the single-threaded job system (deterministic) and thread pools
        struct Config
        {
            bool deterministic;
            int workers;
        };
        for (const Config config : {Config{true, JoltBackend::kWorkerThreads}, Config{false, 1}, Config{false, 4}})
        {
#ifdef N2ENGINE_SINGLE_THREADED
            if (!config.deterministic)
            {
                continue;
            }
#endif
            Scene straight(config.deterministic, config.workers);
            RunRange(straight, 1, kSaveTick, 0);
            const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);
            const std::vector<std::byte> straightEnd = Snapshot(straight.backend);

            for (const int k : {1, 7, 30, 100})
            {
                Scene rolled(config.deterministic, config.workers);
                RunRange(rolled, 1, kSaveTick, 0);
                const std::vector<std::byte> saved = Snapshot(rolled.backend);
                const Hashes atSave = HashOf(rolled.backend);

                RunRange(rolled, kSaveTick + 1, kSaveTick + k, 1); // run on, with another input
                ASSERT_TRUE(rolled.backend.RestoreState(saved)) << "k = " << k;
                EXPECT_EQ(HashOf(rolled.backend), atSave) << "the restore did not give the saved state, k = " << k;
                EXPECT_EQ(Snapshot(rolled.backend), saved) << "the snapshot after a restore is not the one restored, k = " << k;

                const std::vector<Hashes> resumed = Record(rolled, kSaveTick + 1, kLastTick, 0);
                EXPECT_EQ(FirstMismatch(resumed, straightHashes), -1)
                    << "workers " << config.workers << ", rewound " << k << " ticks: the first tick after the save that differs "
                    << "(0-based)";
                EXPECT_EQ(Snapshot(rolled.backend), straightEnd) << "the end state differs byte for byte, k = " << k;
            }
        }
    }

    TEST(JoltJointSnapshotTest, ACorrectedInputAfterTheRestoreEqualsAStraightRunWithIt)
    {
        Scene straight;
        RunRange(straight, 1, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 1);

        for (const int k : {1, 5, 20, 40})
        {
            Scene rolled;
            RunRange(rolled, 1, kSaveTick, 0);
            const std::vector<std::byte> saved = Snapshot(rolled.backend);
            RunRange(rolled, kSaveTick + 1, kSaveTick + k, 0); // predicted with the wrong input
            ASSERT_TRUE(rolled.backend.RestoreState(saved));
            EXPECT_EQ(FirstMismatch(Record(rolled, kSaveTick + 1, kLastTick, 1), straightHashes), -1) << "k = " << k;
        }
    }

    TEST(JoltJointSnapshotTest, SeveralRollbacksInARowStayOnTheStraightRun)
    {
        Scene straight;
        const std::vector<Hashes> straightHashes = Record(straight, 1, kLastTick, 0);

        Scene rolled;
        std::vector<Hashes> hashes;
        // The way a session goes: save every few ticks, sometimes find the prediction wrong and replay
        std::vector<std::byte> saved;
        int savedTick = 0;
        for (int tick = 1; tick <= kLastTick; ++tick)
        {
            if (tick % 10 == 1)
            {
                saved = Snapshot(rolled.backend);
                savedTick = tick - 1;
            }
            if (tick % 23 == 0)
            {
                // Run a few ticks wrong, then go back to the save and replay with the right input up to this tick
                for (int wrong = tick; wrong < tick + 3; ++wrong)
                {
                    Tick(rolled, wrong, 1);
                }
                ASSERT_TRUE(rolled.backend.RestoreState(saved));
                hashes.resize(static_cast<std::size_t>(savedTick));
                for (int replay = savedTick + 1; replay < tick; ++replay)
                {
                    Tick(rolled, replay, 0);
                    hashes.push_back(HashOf(rolled.backend));
                }
            }
            Tick(rolled, tick, 0);
            hashes.push_back(HashOf(rolled.backend));
        }
        EXPECT_EQ(FirstMismatch(hashes, straightHashes), -1);
    }

    TEST(JoltJointSnapshotTest, ARestoreIntoAFreshlyCreatedWorldGivesTheSameFuture)
    {
        Scene first;
        RunRange(first, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(first.backend);
        const std::vector<Hashes> firstHashes = Record(first, kSaveTick + 1, kLastTick, 0);

        // A fresh world straight from its constructor: no step ever ran in it
        {
            Scene fresh;
            ASSERT_TRUE(fresh.backend.RestoreState(saved));
            EXPECT_EQ(FirstMismatch(Record(fresh, kSaveTick + 1, kLastTick, 0), firstHashes), -1);
        }
        // A world with another history, so the restore has real constraint state to undo
        {
            Scene other;
            RunRange(other, 1, 33, 1);
            ASSERT_TRUE(other.backend.RestoreState(saved));
            EXPECT_EQ(FirstMismatch(Record(other, kSaveTick + 1, kLastTick, 0), firstHashes), -1);
        }
        // A world that made the same joints from other poses (every body built 5 units higher): the constraints are made
        // again from the saved settings
        {
            Scene displaced(true, JoltBackend::kWorkerThreads, 5.0f);
            ASSERT_TRUE(displaced.backend.RestoreState(saved));
            EXPECT_EQ(FirstMismatch(Record(displaced, kSaveTick + 1, kLastTick, 0), firstHashes), -1);
        }
    }

    TEST(JoltJointSnapshotTest, AStateSavedUnderAThreadPoolRestoresUnderTheSingleThreadedJobSystemAndTheOtherWayRound)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        GTEST_SKIP() << "the web build has no thread pool";
#else
        Scene single(true, JoltBackend::kWorkerThreads);
        const std::vector<Hashes> expected = Record(single, 1, kLastTick, 0);

        Scene pooled(false, 4);
        RunRange(pooled, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(pooled.backend);
        RunRange(pooled, kSaveTick + 1, kSaveTick + 20, 1);
        ASSERT_TRUE(pooled.backend.RestoreState(saved));
        const std::vector<Hashes> resumed = Record(pooled, kSaveTick + 1, kLastTick, 0);
        EXPECT_EQ(FirstMismatch(resumed, std::vector<Hashes>(expected.begin() + kSaveTick, expected.end())), -1);

        Scene single2(true, JoltBackend::kWorkerThreads);
        ASSERT_TRUE(single2.backend.RestoreState(saved));
        EXPECT_EQ(FirstMismatch(Record(single2, kSaveTick + 1, kLastTick, 0), resumed), -1);
#endif
    }

    // ===== what changes inside the window =====

    TEST(JoltJointSnapshotTest, ALimitChangedInsideTheWindowIsPutBackByTheRestore)
    {
        Scene straight;
        RunRange(straight, 1, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);

        Scene rolled;
        RunRange(rolled, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(rolled.backend);
        JointDesc desc;
        desc.type = JointType::Hinge;
        desc.useLimits = true;
        desc.limitMin = -0.2f;
        desc.limitMax = 0.05f;
        desc.collideConnected = true;
        ASSERT_TRUE(rolled.backend.ConfigureJoint(rolled.elbow, desc));
        RunRange(rolled, kSaveTick + 1, kSaveTick + 20, 0);
        ASSERT_TRUE(rolled.backend.RestoreState(saved));
        EXPECT_EQ(FirstMismatch(Record(rolled, kSaveTick + 1, kLastTick, 0), straightHashes), -1);
    }

    TEST(JoltJointSnapshotTest, AJointThatLeftAndCameBackInsideTheWindowKeepsItsPlaceInTheSolverOrder)
    {
        Scene straight;
        RunRange(straight, 1, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);

        Scene rolled;
        RunRange(rolled, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(rolled.backend);
        // Parking a body and bringing it back takes its joints out of Jolt's list and puts them at its end
        rolled.backend.SetBodyEnabled(rolled.ragdolls[0].bodies[1], false);
        rolled.backend.SetBodyEnabled(rolled.ragdolls[1].bodies[3], false);
        rolled.backend.SetBodyEnabled(rolled.ragdolls[1].bodies[3], true);
        rolled.backend.SetBodyEnabled(rolled.ragdolls[0].bodies[1], true);
        RunRange(rolled, kSaveTick + 1, kSaveTick + 10, 0);
        ASSERT_TRUE(rolled.backend.RestoreState(saved));
        EXPECT_EQ(FirstMismatch(Record(rolled, kSaveTick + 1, kLastTick, 0), straightHashes), -1);
    }

    TEST(JoltJointSnapshotTest, ASceneWithParkedJointedBodiesRoundTripsToo)
    {
        // A ragdoll that is parked (SetBodyEnabled false on every body) for the whole window: its joints are suspended and
        // its constraints keep their state; the other ragdolls run
        const auto park = [](Scene &scene, const bool on)
        {
            for (const PhysicsBodyHandle handle : scene.ragdolls[1].bodies)
            {
                scene.backend.SetBodyEnabled(handle, on);
            }
        };
        Scene straight;
        RunRange(straight, 1, 30, 0);
        park(straight, false);
        RunRange(straight, 31, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);

        Scene rolled;
        RunRange(rolled, 1, 30, 0);
        park(rolled, false);
        RunRange(rolled, 31, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(rolled.backend);
        RunRange(rolled, kSaveTick + 1, kSaveTick + 25, 1);
        ASSERT_TRUE(rolled.backend.RestoreState(saved));
        EXPECT_EQ(FirstMismatch(Record(rolled, kSaveTick + 1, kLastTick, 0), straightHashes), -1);

        // and the parked ragdoll comes back after the window the way it left, on a rolled back world too
        park(rolled, true);
        park(straight, true);
        EXPECT_EQ(FirstMismatch(Record(rolled, kLastTick + 1, kLastTick + 40, 0), Record(straight, kLastTick + 1, kLastTick + 40, 0)), -1);
    }

    // ===== the joint set changes inside the window (stage d) =====

    TEST(JoltJointSnapshotTest, AJointEndedInsideTheWindowIsMadeAgainFromTheSavedStateUnderItsOldHandle)
    {
        Scene straight;
        RunRange(straight, 1, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);
        const std::vector<std::byte> straightFinal = Snapshot(straight.backend);

        Scene scene;
        RunRange(scene, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(scene.backend);
        scene.backend.DestroyJoint(scene.joints[5]);
        RunRange(scene, kSaveTick + 1, kSaveTick + 5, 0);
        ASSERT_FALSE(scene.backend.IsJointValid(scene.joints[5]));
        ASSERT_TRUE(scene.backend.RestoreState(saved));
        EXPECT_TRUE(scene.backend.IsJointValid(scene.joints[5])) << "the joint is back in its slot, with its generation";
        EXPECT_EQ(FirstMismatch(Record(scene, kSaveTick + 1, kLastTick, 0), straightHashes), -1);
        EXPECT_EQ(Snapshot(scene.backend), straightFinal);
    }

    TEST(JoltJointSnapshotTest, AJointMadeOrReplacedInsideTheWindowIsEndedByTheRestore)
    {
        Scene straight;
        RunRange(straight, 1, kSaveTick, 0);
        const std::vector<Hashes> straightHashes = Record(straight, kSaveTick + 1, kLastTick, 0);
        const std::vector<std::byte> straightFinal = Snapshot(straight.backend);

        Scene scene;
        RunRange(scene, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(scene.backend);
        JointDesc desc;
        desc.type = JointType::Ball;
        desc.bodyA = scene.ragdolls[0].bodies[0];
        desc.bodyB = scene.ragdolls[1].bodies[0];
        const PhysicsJointHandle added = scene.backend.CreateJoint(desc);
        ASSERT_TRUE(added.IsValid());
        // and joint 5 replaced by another joint, which takes its slot with the next generation
        scene.backend.DestroyJoint(scene.joints[5]);
        JointDesc other = desc;
        other.bodyA = scene.ragdolls[2].bodies[0];
        other.bodyB = scene.ragdolls[2].bodies[1];
        const PhysicsJointHandle replacement = scene.backend.CreateJoint(other);
        ASSERT_TRUE(replacement.IsValid());
        RunRange(scene, kSaveTick + 1, kSaveTick + 6, 0);
        ASSERT_TRUE(scene.backend.RestoreState(saved));
        EXPECT_FALSE(scene.backend.IsJointValid(added));
        EXPECT_FALSE(scene.backend.IsJointValid(replacement));
        EXPECT_TRUE(scene.backend.IsJointValid(scene.joints[5]));
        EXPECT_EQ(FirstMismatch(Record(scene, kSaveTick + 1, kLastTick, 0), straightHashes), -1);
        EXPECT_EQ(Snapshot(scene.backend), straightFinal);
    }

    TEST(JoltJointSnapshotTest, AnOwnedJointTheRestoreMadeAgainIsEndedAtTheNextStepWhenNobodyClaimsIt)
    {
        ErrorCapture errors;
        Scene scene;
        JointDesc desc;
        desc.type = JointType::Ball;
        desc.bodyA = scene.ragdolls[0].bodies[0];
        desc.bodyB = scene.ragdolls[1].bodies[0];
        desc.owned = true;
        const PhysicsJointHandle owned = scene.backend.CreateJoint(desc);
        ASSERT_TRUE(owned.IsValid());
        const std::vector<std::byte> saved = Snapshot(scene.backend);
        scene.backend.DestroyJoint(owned);
        ASSERT_TRUE(scene.backend.RestoreState(saved));
        ASSERT_TRUE(scene.backend.IsJointValid(owned));
        const PhysicsJointHandle claimed = scene.backend.ClaimJoint(desc);
        EXPECT_EQ(claimed, owned);
        EXPECT_FALSE(scene.backend.ClaimJoint(desc).IsValid()) << "a claimed joint cannot be claimed twice";
        RunRange(scene, 1, 3, 0);
        EXPECT_TRUE(scene.backend.IsJointValid(owned));

        scene.backend.DestroyJoint(owned);
        ASSERT_TRUE(scene.backend.RestoreState(saved));
        RunRange(scene, 3, 5, 0); // nobody claims it
        EXPECT_FALSE(scene.backend.IsJointValid(owned));
    }

    // ===== the refusal (the one impossible case) =====

    TEST(JoltJointSnapshotTest, ASnapshotWhoseJointsBodiesAreNotThereIsRefusedAndChangesNothing)
    {
        ErrorCapture errors;
        Scene scene;
        RunRange(scene, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(scene.backend);
        // A world without the bodies cannot take a snapshot that has them (and so not their joints)
        JoltBackend bare;
        ASSERT_TRUE(bare.Initialize());
        EXPECT_FALSE(bare.RestoreState(saved));
        EXPECT_GE(errors.errors.size(), 1u);
    }

    TEST(JoltJointSnapshotTest, ADamagedJointTableIsRefused)
    {
        ErrorCapture errors;
        Scene scene;
        RunRange(scene, 1, kSaveTick, 0);
        const std::vector<std::byte> saved = Snapshot(scene.backend);
        // Every single byte flipped inside the snapshot is caught by the checksum; a truncated one too
        for (const std::size_t cut : {std::size_t{1}, std::size_t{100}, saved.size() / 2})
        {
            const std::vector<std::byte> shorter(saved.begin(), saved.end() - static_cast<std::ptrdiff_t>(cut));
            EXPECT_FALSE(scene.backend.RestoreState(shorter));
        }
        const Hashes before = HashOf(scene.backend);
        EXPECT_TRUE(scene.backend.RestoreState(saved));
        EXPECT_EQ(HashOf(scene.backend), before) << "restoring the state that is current changed it";
    }

    // ===== the hash covers the joints =====

    TEST(JoltJointSnapshotTest, TheHashesChangeWhenAJointsStateDiffers)
    {
        // Early, while everything is in free fall and awake: a joint call that wakes its bodies changes nothing else
        constexpr int kHashTick = 5;
        const auto world = [](const int ticks)
        {
            auto scene = std::make_unique<Scene>();
            RunRange(*scene, 1, ticks, 0);
            return scene;
        };
        const auto base = world(kHashTick);
        const Hashes reference = HashOf(base->backend);

        // Control: the same scene hashes the same
        EXPECT_EQ(HashOf(world(kHashTick)->backend), reference);

        // A limit differs, with every body where it was
        {
            const auto other = world(kHashTick);
            JointDesc desc;
            desc.type = JointType::Hinge;
            desc.useLimits = true;
            desc.limitMin = -1.2f;
            desc.limitMax = 0.31f;
            // the same bodies' poses and velocities: ConfigureJoint wakes the bodies of the joint, which are asleep or not
            // as they were, so compare the simulated hash of a state restored over it
            const std::vector<std::byte> saved = Snapshot(other->backend);
            ASSERT_TRUE(other->backend.ConfigureJoint(other->elbow, desc));
            ASSERT_TRUE(other->backend.RestoreState(saved)); // puts the limit back
            EXPECT_EQ(HashOf(other->backend), reference);
            ASSERT_TRUE(other->backend.ConfigureJoint(other->elbow, desc));
            const Hashes changed = HashOf(other->backend);
            EXPECT_NE(changed.full, reference.full) << "a changed limit does not change HashState";
            EXPECT_NE(changed.simulated, reference.simulated) << "a changed limit does not change HashSimulatedState";
        }
        // collideConnected differs
        {
            const auto other = world(kHashTick);
            JointDesc desc;
            desc.type = JointType::Hinge;
            desc.useLimits = true;
            desc.limitMin = -1.2f;
            desc.limitMax = 0.3f;
            desc.collideConnected = true;
            ASSERT_TRUE(other->backend.ConfigureJoint(other->elbow, desc));
            EXPECT_NE(HashOf(other->backend).full, reference.full);
            EXPECT_NE(HashOf(other->backend).simulated, reference.simulated);
        }
        // a joint is gone
        {
            const auto other = world(kHashTick);
            other->backend.DestroyJoint(other->joints[0]);
            EXPECT_NE(HashOf(other->backend).full, reference.full);
            EXPECT_NE(HashOf(other->backend).simulated, reference.simulated);
        }
        // a joint is suspended with its body, which parks it: bodies differ too, so look at the joints alone with a ragdoll
        // whose bodies were parked in both worlds
        {
            Scene a;
            Scene b;
            for (Scene *scene : {&a, &b})
            {
                RunRange(*scene, 1, 20, 0);
                for (const PhysicsBodyHandle handle : scene->ragdolls[1].bodies)
                {
                    scene->backend.SetBodyEnabled(handle, false);
                }
            }
            EXPECT_EQ(HashOf(a.backend), HashOf(b.backend));
            b.backend.DestroyJoint(b.ragdolls[1].joints[0]); // a suspended joint of the parked ragdoll
            EXPECT_NE(HashOf(a.backend).full, HashOf(b.backend).full) << "HashState covers suspended joints";
        }
    }

    TEST(JoltJointSnapshotTest, TheHashOfASceneWithoutJointsIsWhatItWasBeforeJointsExisted)
    {
        // Nothing joint-related is mixed in when there are no joints: the golden hashes of the determinism corpus stay
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        const PhysicsBodyHandle body = backend.CreateDynamicBody(Vector3(0.0f, 1.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
        backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
        const Hashes before = HashOf(backend);

        const PhysicsBodyHandle other = backend.CreateDynamicBody(Vector3(0.0f, 3.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
        backend.AddSphereCollider(other, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
        JointDesc desc;
        desc.type = JointType::Distance;
        desc.bodyA = body;
        desc.bodyB = other;
        const PhysicsJointHandle joint = backend.CreateJoint(desc);
        ASSERT_TRUE(joint.IsValid());
        const Hashes withJoint = HashOf(backend);
        backend.DestroyJoint(joint);
        backend.DestroyBody(other);
        EXPECT_EQ(HashOf(backend).simulated, before.simulated);
        EXPECT_NE(withJoint.simulated, before.simulated);
    }
}

#endif // N2ENGINE_JOLT_ENABLED
