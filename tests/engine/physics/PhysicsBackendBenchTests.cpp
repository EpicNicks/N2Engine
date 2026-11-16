// Timing of the Jolt and PhysX backends on one scene (issue #381 item 7): the step cost of a 100-body pile and the cost
// of a Jolt snapshot (SaveState, RestoreState, HashState). It asserts nothing about speed, since a CI machine's timing
// is noise; it asserts that every operation works and prints "[BENCH]" lines, which orchestrator/proposals/
// physics-bench-381.md quotes. Both backends run single-threaded (physics.deterministic), the netcode configuration.

#include "PhysicsTestFixture.hpp"

#if defined(N2ENGINE_JOLT_ENABLED) || defined(N2ENGINE_PHYSX_ENABLED)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/physics/PhysicsMaterial.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;
        constexpr int kWarmupTicks = 120; // the pile has landed and is mostly settling
        constexpr int kMeasuredTicks = 300;
        constexpr int kSnapshotRepeats = 200;

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

        void BuildPile(IPhysicsBackend &backend, const int count)
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
        }

        void Tick(IPhysicsBackend &backend)
        {
            backend.ApplyPendingChanges();
            backend.Update(kStep);
            backend.SyncTransforms();
            backend.ProcessCollisionCallbacks();
        }

        // Median and worst step in microseconds over kMeasuredTicks
        void MeasureSteps(const char *name, IPhysicsBackend &backend, const int bodies)
        {
            BuildPile(backend, bodies);
            for (int i = 0; i < kWarmupTicks; ++i)
            {
                Tick(backend);
            }
            std::vector<double> times;
            for (int i = 0; i < kMeasuredTicks; ++i)
            {
                const Clock::time_point start = Clock::now();
                Tick(backend);
                times.push_back(Micros(start, Clock::now()));
            }
            const double worst = *std::max_element(times.begin(), times.end());
            std::printf("[BENCH] step %s bodies=%d median_us=%.1f worst_us=%.1f\n", name, bodies, Median(times), worst);
            std::fflush(stdout);
        }
    }

#ifdef N2ENGINE_JOLT_ENABLED
    TEST(PhysicsBackendBenchTest, JoltStepAndSnapshotCost)
    {
        for (const int bodies : {25, 100, 400})
        {
            JoltBackend backend;
            backend.SetDeterministic(true);
            ASSERT_TRUE(backend.Initialize());
            MeasureSteps("jolt", backend, bodies);

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
            std::printf("[BENCH] snapshot jolt bodies=%d bytes=%zu save_median_us=%.1f restore_median_us=%.1f "
                        "hash_median_us=%.1f\n",
                        bodies, saved.size(), Median(save), Median(restore), Median(hash));
            std::fflush(stdout);
        }
    }
#endif

#ifdef N2ENGINE_PHYSX_ENABLED
    TEST(PhysicsBackendBenchTest, PhysXStepCost)
    {
        for (const int bodies : {25, 100, 400})
        {
            PhysXBackend backend;
            backend.SetDeterministic(true);
            ASSERT_TRUE(backend.Initialize());
            MeasureSteps("physx", backend, bodies);
        }
    }
#endif
}

#endif
