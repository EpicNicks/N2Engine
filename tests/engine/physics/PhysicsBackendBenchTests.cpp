// Timing of the Jolt and PhysX backends (issue #381 item 7): the step cost of a settled pile, of a pile kept awake by a
// kinematic sweeper and of hanging joint chains, the cost of 1000 raycasts, and the cost of a Jolt snapshot (SaveState,
// RestoreState, HashState) of each. It asserts nothing about speed, since a CI machine's timing is noise; it asserts that
// every operation works and prints "[BENCH]" lines of key=value pairs, which CI collects into the job summary of every leg
// that builds the engine tests (scripts/physics-bench-summary.py) and orchestrator/proposals/physics-bench-381.md quotes.
// Both backends run single-threaded (physics.deterministic), the netcode configuration.

#include "PhysicsTestFixture.hpp"

#if defined(N2ENGINE_JOLT_ENABLED) || defined(N2ENGINE_PHYSX_ENABLED)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/Layers.hpp"
#include "engine/physics/JointTypes.hpp"
#include "engine/physics/PhysicsFeatures.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;
        constexpr int kWarmupTicks = 120; // the pile has landed and is mostly settling
        constexpr int kMeasuredTicks = 300;
        constexpr int kSnapshotRepeats = 200;
        constexpr int kRays = 1000;

        using Clock = std::chrono::steady_clock;

        double Micros(const Clock::time_point from, const Clock::time_point to)
        {
            return std::chrono::duration<double, std::micro>(to - from).count();
        }

        double Median(std::vector<double> values)
        {
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        /// The scene of one measurement: "pile" settles and mostly sleeps, "active" has a kinematic sweeper pushing
        /// through it every tick (so it stays awake), "joints" is hanging hinge chains that swing into each other
        enum class BenchScene
        {
            Pile,
            Active,
            Joints
        };

        const char *Name(const BenchScene scene)
        {
            switch (scene)
            {
            case BenchScene::Pile:
                return "pile";
            case BenchScene::Active:
                return "active";
            default:
                return "joints";
            }
        }

        struct Built
        {
            PhysicsBodyHandle sweeper;
            int bodies = 0;
            int joints = 0;
        };

        void BuildPile(IPhysicsBackend &backend, const int count, Built &built)
        {
            const PhysicsMaterial material = PhysicsMaterial::Default();
            const PhysicsBodyHandle floor =
                backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), Math::Quaternion::Identity, nullptr);
            backend.AddBoxCollider(floor, nullptr, Vector3(50.0f, 0.5f, 50.0f), Vector3::Zero, material);
            for (int i = 0; i < count; ++i)
            {
                const int column = i % 5;
                const int row = (i / 5) % 5;
                const int layer = i / 25;
                const float x = 1.3f * static_cast<float>(column - 2) + 0.05f * static_cast<float>(i % 7);
                const float y = 1.0f + 1.6f * static_cast<float>(layer);
                const float z = 1.3f * static_cast<float>(row - 2) + 0.05f * static_cast<float>(i % 3);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(x, y, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                switch (i % 3)
                {
                case 0:
                    backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                    break;
                case 1:
                    backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, material);
                    break;
                default:
                    backend.AddCapsuleCollider(body, nullptr, 0.3f, 1.2f, Vector3::Zero, material);
                    break;
                }
            }
            built.bodies = count;
        }

        /// `count` bodies rounded down to whole chains of 10 hinged links (at least one chain), hanging straight from world
        /// points on a grid 1 apart (the links are 0.3 wide, so none overlaps at the start); each chain's last link is
        /// pushed sideways, so the chains swing into each other
        void BuildJointChains(IPhysicsBackend &backend, const int count, Built &built)
        {
            const PhysicsMaterial material = PhysicsMaterial::Default();
            const PhysicsBodyHandle floor =
                backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), Math::Quaternion::Identity, nullptr);
            backend.AddBoxCollider(floor, nullptr, Vector3(50.0f, 0.5f, 50.0f), Vector3::Zero, material);
            const int chains = std::max(1, count / 10);
            for (int chain = 0; chain < chains; ++chain)
            {
                const float x = -10.0f + 1.0f * static_cast<float>(chain % 20);
                const float z = 1.0f * static_cast<float>(chain / 20);
                PhysicsBodyHandle previous;
                Vector3 previousPos;
                for (int link = 0; link < 10; ++link)
                {
                    const Vector3 position(x, 12.0f - 0.8f * static_cast<float>(link), z);
                    const PhysicsBodyHandle body =
                        backend.CreateDynamicBody(position, Math::Quaternion::Identity, 1.0f, nullptr, false);
                    backend.AddBoxCollider(body, nullptr, Vector3(0.15f, 0.35f, 0.15f), Vector3::Zero, material);
                    JointDesc desc;
                    desc.type = link == 0 ? JointType::Ball : JointType::Hinge;
                    desc.bodyA = body;
                    desc.anchorA = Vector3(0.0f, 0.4f, 0.0f);
                    if (link == 0)
                    {
                        desc.anchorB = position + Vector3(0.0f, 0.4f, 0.0f); // the world point
                    }
                    else
                    {
                        desc.bodyB = previous;
                        desc.anchorB = position + Vector3(0.0f, 0.4f, 0.0f) - previousPos;
                        desc.axis = Vector3(0.0f, 0.0f, 1.0f);
                    }
                    EXPECT_TRUE(backend.CreateJoint(desc).IsValid());
                    ++built.joints;
                    previous = body;
                    previousPos = position;
                    ++built.bodies;
                }
                backend.SetVelocity(previous, Vector3(2.0f * static_cast<float>(chain % 3 - 1), 0.0f, 1.0f * static_cast<float>(chain % 2)));
            }
        }

        bool Build(IPhysicsBackend &backend, const BenchScene scene, const int count, Built &built)
        {
            if (scene == BenchScene::Joints)
            {
                if (!backend.GetImplementedFeatures().Has(PhysicsFeature::Joints))
                {
                    return false;
                }
                BuildJointChains(backend, count, built);
                return true;
            }
            BuildPile(backend, count, built);
            if (scene == BenchScene::Active)
            {
                built.sweeper = backend.CreateDynamicBody(Vector3(-8.0f, 1.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, true);
                backend.AddBoxCollider(built.sweeper, nullptr, Vector3(0.4f, 1.0f, 4.0f), Vector3::Zero, PhysicsMaterial::Default());
            }
            return true;
        }

        void Tick(IPhysicsBackend &backend, const Built &built, const int tick)
        {
            if (built.sweeper.IsValid())
            {
                // Back and forth through the pile, 16 units in 4 seconds
                const int phase = tick % 480;
                const float t = static_cast<float>(phase < 240 ? phase : 480 - phase) / 240.0f;
                backend.SetBodyTransform(built.sweeper, Vector3(-8.0f + 16.0f * t, 1.0f, 0.0f), Math::Quaternion::Identity);
            }
            backend.ApplyPendingChanges();
            backend.Update(kStep);
            backend.SyncTransforms();
            backend.ProcessCollisionCallbacks();
        }

        /// Builds the scene, runs the warm-up and prints the median and worst step in microseconds over kMeasuredTicks.
        /// False when the backend cannot build the scene (no joints).
        bool MeasureSteps(const char *name, IPhysicsBackend &backend, const BenchScene scene, const int count, Built &built)
        {
            if (!Build(backend, scene, count, built))
            {
                std::printf("[BENCH] kind=step backend=%s scene=%s bodies=%d skipped=no-joints\n", name, Name(scene), count);
                std::fflush(stdout);
                return false;
            }
            int tick = 0;
            for (int i = 0; i < kWarmupTicks; ++i)
            {
                Tick(backend, built, ++tick);
            }
            std::vector<double> times;
            for (int i = 0; i < kMeasuredTicks; ++i)
            {
                const Clock::time_point start = Clock::now();
                Tick(backend, built, ++tick);
                times.push_back(Micros(start, Clock::now()));
            }
            const double worst = *std::max_element(times.begin(), times.end());
            std::printf("[BENCH] kind=step backend=%s scene=%s bodies=%d joints=%d median_us=%.1f worst_us=%.1f\n", name,
                        Name(scene), built.bodies, built.joints, Median(times), worst);
            std::fflush(stdout);
            return true;
        }

        /// kRays downward rays over the pile's footprint, timed as one batch (the median of 5 batches)
        void MeasureRaycasts(const char *name, IPhysicsBackend &backend, const int bodies)
        {
            std::vector<double> batches;
            int hits = 0;
            for (int batch = 0; batch < 5; ++batch)
            {
                hits = 0;
                const Clock::time_point start = Clock::now();
                for (int i = 0; i < kRays; ++i)
                {
                    const float x = -4.0f + 8.0f * static_cast<float>(i % 40) / 40.0f;
                    const float z = -4.0f + 8.0f * static_cast<float>(i / 40) / 25.0f;
                    RaycastHit hit;
                    if (backend.Raycast(Vector3(x, 60.0f, z), Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f, Layers::DefaultRaycastMask,
                                        QueryTriggers::Collide))
                    {
                        ++hits;
                    }
                }
                batches.push_back(Micros(start, Clock::now()));
            }
            EXPECT_GT(hits, 0) << name << ": no ray hit the pile";
            std::printf("[BENCH] kind=raycast backend=%s scene=pile bodies=%d rays=%d hits=%d median_us=%.1f\n", name, bodies,
                        kRays, hits, Median(batches));
            std::fflush(stdout);
        }
    }

#ifdef N2ENGINE_JOLT_ENABLED
    TEST(PhysicsBackendBenchTest, JoltStepAndSnapshotCost)
    {
        for (const BenchScene scene : {BenchScene::Pile, BenchScene::Active, BenchScene::Joints})
        {
            for (const int bodies : {25, 100, 400})
            {
                JoltBackend backend;
                backend.SetDeterministic(true);
                ASSERT_TRUE(backend.Initialize());
                Built built;
                ASSERT_TRUE(MeasureSteps("jolt", backend, scene, bodies, built)) << "Jolt has joints";
                if (scene == BenchScene::Pile)
                {
                    MeasureRaycasts("jolt", backend, built.bodies);
                }

                std::vector<std::byte> saved;
                std::vector<double> save, restore, hash;
                for (int i = 0; i < kSnapshotRepeats; ++i)
                {
                    Clock::time_point t = Clock::now();
                    ASSERT_TRUE(backend.SaveState(saved));
                    save.push_back(Micros(t, Clock::now()));
                    t = Clock::now();
                    ASSERT_TRUE(backend.RestoreState(saved));
                    restore.push_back(Micros(t, Clock::now()));
                    t = Clock::now();
                    ASSERT_TRUE(backend.HashState().has_value());
                    hash.push_back(Micros(t, Clock::now()));
                }
                std::printf("[BENCH] kind=snapshot backend=jolt scene=%s bodies=%d joints=%d bytes=%zu save_median_us=%.1f "
                            "restore_median_us=%.1f hash_median_us=%.1f\n",
                            Name(scene), built.bodies, built.joints, saved.size(), Median(save), Median(restore), Median(hash));
                std::fflush(stdout);
            }
        }
    }
#endif

#ifdef N2ENGINE_PHYSX_ENABLED
    TEST(PhysicsBackendBenchTest, PhysXStepCost)
    {
        for (const BenchScene scene : {BenchScene::Pile, BenchScene::Active, BenchScene::Joints})
        {
            for (const int bodies : {25, 100, 400})
            {
                PhysXBackend backend;
                backend.SetDeterministic(true);
                ASSERT_TRUE(backend.Initialize());
                Built built;
                if (MeasureSteps("physx", backend, scene, bodies, built) && scene == BenchScene::Pile)
                {
                    MeasureRaycasts("physx", backend, built.bodies);
                }
            }
        }
    }
#endif
}

#endif
