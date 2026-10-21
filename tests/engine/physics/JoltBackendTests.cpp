// What is specific to the Jolt backend (stage J3): its declarations, its job system choice, the process-wide state it
// shares between backends, the "not implemented yet" members, the body ids, enable and disable, set-state, and the
// determinism smoke (a scene stepped twice in one process gives one state hash; the hashes are printed for the CI
// step "Jolt backend determinism numbers"). The behaviour both backends share is PhysicsCoreTests.cpp.

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr bool kSingleThreadedBuild =
#ifdef N2ENGINE_SINGLE_THREADED
            true;
#else
            false;
#endif

        constexpr bool kStrictFpBuild =
#ifdef N2_STRICT_FP
            true;
#else
            false;
#endif

        constexpr bool kCrossPlatformJolt =
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
            true;
#else
            false;
#endif

        // What the build is, for the log line a CI step greps
        const char *BuildDescription()
        {
#if defined(__EMSCRIPTEN__)
            return "emscripten wasm32";
#elif defined(_MSC_VER) && defined(_M_X64)
            return "msvc x86_64";
#elif defined(__clang__) && defined(__x86_64__)
            return "clang x86_64";
#elif defined(__clang__) && defined(__aarch64__)
            return "clang arm64";
#elif defined(__GNUC__) && defined(__x86_64__)
            return "gcc x86_64";
#elif defined(__GNUC__) && defined(__aarch64__)
            return "gcc arm64";
#else
            return "other";
#endif
        }

        Math::Quaternion TurnedRotation()
        {
            // A unit rotation about no particular axis: (0.099, 0.198, 0.297) and w = sqrt(1 - 0.1372)
            return Math::Quaternion(0.92887f, 0.099f, 0.198f, 0.297f);
        }

        // FNV-1a over the bytes of 32-bit words: no std::hash, no library call whose result may differ between platforms
        void HashWord(std::uint64_t &hash, const std::uint32_t word)
        {
            for (int i = 0; i < 4; ++i)
            {
                hash ^= static_cast<std::uint64_t>((word >> (8 * i)) & 0xFFu);
                hash *= 1099511628211ULL;
            }
        }

        void HashFloat(std::uint64_t &hash, const float value)
        {
            std::uint32_t word = 0;
            std::memcpy(&word, &value, sizeof(word));
            HashWord(hash, word);
        }

        // The states of the bodies in handle order, as the bits of the floats
        std::uint64_t HashBodies(IPhysicsBackend &backend, const std::vector<PhysicsBodyHandle> &bodies, bool &finite)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            finite = true;
            for (const PhysicsBodyHandle handle : bodies)
            {
                BodyState state;
                if (!backend.GetBodyState(handle, state))
                {
                    HashWord(hash, 0xDEADu); // a disabled body is part of the state too
                    continue;
                }
                const float values[] = {state.position.x,        state.position.y,        state.position.z,
                                        state.rotation.GetX(),   state.rotation.GetY(),   state.rotation.GetZ(),
                                        state.rotation.GetW(),   state.linearVelocity.x,  state.linearVelocity.y,
                                        state.linearVelocity.z,  state.angularVelocity.x, state.angularVelocity.y,
                                        state.angularVelocity.z};
                for (const float value : values)
                {
                    finite = finite && std::isfinite(value);
                    HashFloat(hash, value);
                }
                HashWord(hash, state.sleeping ? 1u : 0u);
            }
            return hash;
        }

        struct SceneResult
        {
            bool finite = false;
            std::size_t bodies = 0;
            std::uint64_t hashAfter60 = 0;
            std::uint64_t hash = 0;
            float ballHeight = 0.0f;
            float platformX = 0.0f;
        };

        // A scene that uses most of the core through the backend API alone (no engine components, so nothing but the
        // backend's own state is in the result): a floor, a leaning stack, a turned box, rolling and landing spheres, a
        // capsule, a compound body with a mass of its own, a kinematic platform that carries a ball, an impulse at step
        // 30, a body disabled at step 90 and enabled at step 120, SetBodyState at step 150 and a gravity change at step
        // 200. 240 steps of 1/60 s.
        SceneResult RunScene(const bool deterministic)
        {
            JoltBackend backend;
            backend.SetDeterministic(deterministic);
            EXPECT_TRUE(backend.Initialize());

            const PhysicsMaterial material = PhysicsMaterial::Default();
            std::vector<PhysicsBodyHandle> bodies;
            const Math::Quaternion identity = Math::Quaternion::Identity;

            const PhysicsBodyHandle floor = backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), identity, nullptr);
            backend.AddBoxCollider(floor, nullptr, Vector3(30.0f, 0.5f, 30.0f), Vector3::Zero, material);
            bodies.push_back(floor);

            std::vector<PhysicsBodyHandle> stack;
            for (int i = 0; i < 3; ++i)
            {
                const float fi = static_cast<float>(i);
                const PhysicsBodyHandle box =
                    backend.CreateDynamicBody(Vector3(-3.0f + 0.05f * fi, 0.5f + 1.02f * fi, 0.0f), identity, 1.0f, nullptr, false);
                backend.AddBoxCollider(box, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                bodies.push_back(box);
                stack.push_back(box);
            }

            const PhysicsBodyHandle turned = backend.CreateDynamicBody(Vector3(3.0f, 2.0f, 0.0f), TurnedRotation(), 2.0f, nullptr, false);
            backend.AddBoxCollider(turned, nullptr, Vector3(0.6f, 0.4f, 0.5f), Vector3::Zero, material);
            bodies.push_back(turned);

            const PhysicsBodyHandle roller = backend.CreateDynamicBody(Vector3(0.0f, 3.0f, 0.0f), identity, 1.0f, nullptr, false);
            backend.AddSphereCollider(roller, nullptr, 0.5f, Vector3::Zero, material);
            backend.SetVelocity(roller, Vector3(2.0f, 0.0f, 1.0f));
            bodies.push_back(roller);

            const PhysicsBodyHandle lander = backend.CreateDynamicBody(Vector3(3.2f, 5.0f, 0.1f), identity, 0.5f, nullptr, false);
            backend.AddSphereCollider(lander, nullptr, 0.4f, Vector3::Zero, material);
            bodies.push_back(lander);

            const PhysicsBodyHandle capsule = backend.CreateDynamicBody(Vector3(6.0f, 3.0f, 0.0f), TurnedRotation(), 1.5f, nullptr, false);
            backend.AddCapsuleCollider(capsule, nullptr, 0.3f, 1.2f, Vector3::Zero, material);
            bodies.push_back(capsule);

            const PhysicsBodyHandle compound = backend.CreateDynamicBody(Vector3(9.0f, 3.0f, 0.0f), identity, 3.0f, nullptr, false);
            backend.AddBoxCollider(compound, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
            backend.AddSphereCollider(compound, nullptr, 0.5f, Vector3(1.5f, 0.0f, 0.0f), material);
            bodies.push_back(compound);

            const PhysicsBodyHandle platform = backend.CreateDynamicBody(Vector3(-8.0f, 1.0f, 4.0f), identity, 1.0f, nullptr, true);
            backend.AddBoxCollider(platform, nullptr, Vector3(2.0f, 0.2f, 2.0f), Vector3::Zero, material);
            bodies.push_back(platform);

            const PhysicsBodyHandle passenger = backend.CreateDynamicBody(Vector3(-8.0f, 2.0f, 4.0f), identity, 1.0f, nullptr, false);
            backend.AddSphereCollider(passenger, nullptr, 0.4f, Vector3::Zero, material);
            bodies.push_back(passenger);

            SceneResult result;
            result.bodies = bodies.size();
            const float step = 1.0f / 60.0f;
            for (int i = 1; i <= 240; ++i)
            {
                backend.SetBodyTransform(platform, Vector3(-8.0f + 0.02f * static_cast<float>(i), 1.0f, 4.0f), identity);
                if (i == 30)
                {
                    backend.AddImpulse(stack[2], Vector3(1.5f, 0.0f, 0.0f));
                }
                if (i == 90)
                {
                    backend.SetBodyEnabled(stack[1], false);
                }
                if (i == 120)
                {
                    backend.SetBodyEnabled(stack[1], true);
                }
                if (i == 150)
                {
                    BodyState state;
                    if (backend.GetBodyState(lander, state))
                    {
                        state.position = state.position + Vector3(0.0f, 1.0f, 0.0f);
                        state.linearVelocity = Vector3(0.0f, 0.0f, 0.0f);
                        backend.SetBodyState(lander, state);
                    }
                }
                if (i == 200)
                {
                    backend.SetGravity(Vector3(0.5f, -9.81f, 0.0f));
                }
                backend.ApplyPendingChanges();
                backend.Update(step);
                backend.SyncTransforms();
                if (i == 60)
                {
                    bool finite60 = false;
                    result.hashAfter60 = HashBodies(backend, bodies, finite60);
                }
            }
            result.hash = HashBodies(backend, bodies, result.finite);
            result.ballHeight = backend.GetPosition(roller).y;
            result.platformX = backend.GetPosition(platform).x;
            return result;
        }

        unsigned long long Printable(const std::uint64_t value)
        {
            return static_cast<unsigned long long>(value);
        }

        void PrintHash(const char *jobs, const SceneResult &result)
        {
            std::printf("[JOLT-BACKEND-DETERMINISM] build: %s, jobs: %s, level: %s, scene: mixed, bodies: %zu, hash60: "
                        "0x%016llx, hash240: 0x%016llx\n",
                        BuildDescription(), jobs, std::string(ToString(JoltBackend::DeclaredDeterminism(true))).c_str(),
                        result.bodies, Printable(result.hashAfter60), Printable(result.hash));
            std::fflush(stdout);
        }
    }

    // ===== declarations =====

    TEST(JoltBackendTest, DeclaresItself)
    {
        JoltBackend backend;
        EXPECT_EQ(backend.BackendId(), "jolt");
        EXPECT_TRUE(backend.SupportsSetState());
        EXPECT_FALSE(backend.SupportsSnapshot());
        EXPECT_EQ(JoltBackend::kSupportsSetState, backend.SupportsSetState());
        EXPECT_EQ(JoltBackend::kSupportsSnapshot, backend.SupportsSnapshot());
        EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled);
        EXPECT_EQ(backend.GetParticleBackend(), nullptr);
        EXPECT_EQ(backend.GetSoftBodyBackend(), nullptr);
    }

    TEST(JoltBackendTest, TheBuildIsTheCrossPlatformDeterministicOne)
    {
        EXPECT_TRUE(kCrossPlatformJolt) << "Jolt::Jolt always carries JPH_CROSS_PLATFORM_DETERMINISTIC";
    }

    TEST(JoltBackendTest, DeterminismDeclarationFollowsTheJobSystemAndTheBuild)
    {
        for (const bool deterministic : {false, true})
        {
            JoltBackend backend;
            backend.SetDeterministic(deterministic);
            const DeterminismLevel before = backend.GetDeterminism();
            EXPECT_EQ(before, JoltBackend::DeclaredDeterminism(deterministic));
            ASSERT_TRUE(backend.Initialize());
            EXPECT_EQ(backend.GetDeterminism(), before) << "the declaration changed with Initialize";

            const bool singleThreaded = deterministic || kSingleThreadedBuild;
            EXPECT_EQ(backend.UsesSingleThreadedJobSystem(), singleThreaded);
            if (!singleThreaded)
            {
                // The thread pool: nothing is promised until thread-count independence is proven in CI
                EXPECT_EQ(before, DeterminismLevel::None);
            }
            else if (kCrossPlatformJolt && kStrictFpBuild)
            {
                EXPECT_EQ(before, DeterminismLevel::CrossPlatform);
            }
            else
            {
                EXPECT_EQ(before, DeterminismLevel::SameBinary);
            }
            // CrossPlatform only with every condition: single-threaded, cross-platform Jolt, the strict profile
            EXPECT_EQ(before == DeterminismLevel::CrossPlatform, singleThreaded && kCrossPlatformJolt && kStrictFpBuild);
        }
    }

    // ===== Jolt's process-wide state =====

    TEST(JoltBackendTest, GlobalStateIsSharedByBackendsAndReleasedByTheLast)
    {
        const int baseline = JoltBackend::GlobalInitCount();
        {
            JoltBackend first;
            JoltBackend second;
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline) << "a backend holds the state only once initialized";
            ASSERT_TRUE(first.Initialize());
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline + 1);
            ASSERT_TRUE(second.Initialize());
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline + 2);
            EXPECT_TRUE(first.Initialize()) << "initializing twice is harmless";
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline + 2);

            first.Shutdown();
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline + 1);
            // The second backend still works after the first one left
            const PhysicsBodyHandle body =
                second.CreateDynamicBody(Vector3(0.0f, 10.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
            ASSERT_TRUE(body.IsValid());
            second.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
            for (int i = 0; i < 10; ++i)
            {
                second.Update(1.0f / 60.0f);
            }
            EXPECT_LT(second.GetPosition(body).y, 10.0f);
            first.Shutdown(); // twice is harmless
            EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline + 1);
        }
        EXPECT_EQ(JoltBackend::GlobalInitCount(), baseline) << "the state was not released";
    }

    TEST(JoltBackendTest, ABackendCanBeInitializedAgainAfterShutdown)
    {
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        backend.Shutdown();
        EXPECT_FALSE(backend.CreateStaticBody(Vector3::Zero, Math::Quaternion::Identity, nullptr).IsValid())
            << "a backend that is shut down makes no bodies";
        ASSERT_TRUE(backend.Initialize());
        EXPECT_TRUE(backend.CreateStaticBody(Vector3::Zero, Math::Quaternion::Identity, nullptr).IsValid());
    }

    // ===== members that are not implemented yet =====

    TEST(JoltBackendTest, UnimplementedMembersWarnOnceAndNeverCrash)
    {
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        // Initialize warns once that events are not delivered
        EXPECT_EQ(backend.GetNotImplementedWarningCount(), 1u);

        RaycastHit hit;
        std::vector<RaycastHit> hits(3);
        EXPECT_FALSE(backend.Raycast(Vector3::Zero, Vector3(0.0f, -1.0f, 0.0f), hit, 10.0f, ~0u, QueryTriggers::Collide));
        EXPECT_EQ(backend.RaycastAll(Vector3::Zero, Vector3(0.0f, -1.0f, 0.0f), hits, 10.0f, ~0u, QueryTriggers::Collide),
                  0);
        EXPECT_TRUE(hits.empty());
        EXPECT_FALSE(backend.SphereCast(Vector3::Zero, 0.5f, Vector3(0.0f, -1.0f, 0.0f), hit, 10.0f, ~0u));
        EXPECT_EQ(backend.GetNotImplementedWarningCount(), 2u) << "three query calls, one warning";

        backend.RefreshCollisionMatrix();
        backend.SetColliderLayer(INVALID_PHYSICS_HANDLE, nullptr, 3);
        EXPECT_EQ(backend.GetNotImplementedWarningCount(), 3u) << "two layer calls, one warning";

        std::vector<std::byte> bytes{std::byte{1}};
        EXPECT_FALSE(backend.SaveState(bytes));
        EXPECT_FALSE(backend.RestoreState(bytes));
        EXPECT_FALSE(backend.HashState().has_value());
        EXPECT_EQ(backend.GetNotImplementedWarningCount(), 4u) << "three snapshot calls, one warning";

        backend.ProcessCollisionCallbacks(); // nothing to do, nothing to say
        EXPECT_EQ(backend.GetNotImplementedWarningCount(), 4u);
    }

    // ===== body ids =====

    TEST(JoltBackendTest, HandlesAndJoltIdsFollowTheDeterministicFreeList)
    {
        // The same sequence of creations and destructions gives the same handles and the same Jolt ids on every
        // backend instance (and, when PhysX is in the build, the same handles as PhysX), so ids match across peers
        const auto run = []<typename B>(B &backend)
        {
            std::vector<std::uint32_t> trace;
            const auto record = [&trace](const PhysicsBodyHandle h)
            {
                trace.push_back(h.index);
                trace.push_back(h.generation);
            };
            const Math::Quaternion identity = Math::Quaternion::Identity;
            const PhysicsBodyHandle a = backend.CreateStaticBody(Vector3(0.0f, 0.0f, 0.0f), identity, nullptr);
            const PhysicsBodyHandle b = backend.CreateDynamicBody(Vector3(2.0f, 0.0f, 0.0f), identity, 1.0f, nullptr, false);
            const PhysicsBodyHandle c = backend.CreateDynamicBody(Vector3(4.0f, 0.0f, 0.0f), identity, 1.0f, nullptr, true);
            record(a);
            record(b);
            record(c);
            backend.DestroyBody(b);
            const PhysicsBodyHandle d = backend.CreateStaticBody(Vector3(6.0f, 0.0f, 0.0f), identity, nullptr);
            backend.DestroyBody(a);
            backend.DestroyBody(c);
            const PhysicsBodyHandle e = backend.CreateStaticBody(Vector3(8.0f, 0.0f, 0.0f), identity, nullptr);
            const PhysicsBodyHandle f = backend.CreateStaticBody(Vector3(10.0f, 0.0f, 0.0f), identity, nullptr);
            const PhysicsBodyHandle g = backend.CreateStaticBody(Vector3(12.0f, 0.0f, 0.0f), identity, nullptr);
            record(d);
            record(e);
            record(f);
            record(g);
            return trace;
        };

        JoltBackend first;
        ASSERT_TRUE(first.Initialize());
        const std::vector<std::uint32_t> expected = run(first);
        ASSERT_EQ(expected.size(), 14u);
        // b's slot was reused by d with the next generation; c's slot (freed last) is the first reused after
        EXPECT_EQ(expected[6], expected[2]);
        EXPECT_EQ(expected[7], expected[3] + 1);

        JoltBackend second;
        ASSERT_TRUE(second.Initialize());
        EXPECT_EQ(run(second), expected);

#ifdef N2ENGINE_PHYSX_ENABLED
        PhysXBackend physx;
        ASSERT_TRUE(physx.Initialize());
        EXPECT_EQ(run(physx), expected) << "PhysX and Jolt hand out different handles for one sequence";
#endif

        // The Jolt id of a live body is the handle's index
        const PhysicsBodyHandle live = first.CreateStaticBody(Vector3::Zero, Math::Quaternion::Identity, nullptr);
        EXPECT_EQ(first.GetJoltBodyIndex(live), live.index);
        first.DestroyBody(live);
        EXPECT_EQ(first.GetJoltBodyIndex(live), UINT32_MAX) << "a stale handle has no body";
    }

    TEST(JoltBackendTest, RefusesABodyBeyondTheLimit)
    {
        // The limit (JoltBackend::kMaxBodies, 10240) is lowered for the test, so it does not need ten thousand bodies
        JoltBackend backend;
        backend.SetMaxBodiesForTesting(8);
        ASSERT_TRUE(backend.Initialize());
        std::vector<PhysicsBodyHandle> bodies;
        for (int i = 0; i < 8; ++i)
        {
            const PhysicsBodyHandle body =
                backend.CreateStaticBody(Vector3(static_cast<float>(i) * 2.0f, 0.0f, 0.0f), Math::Quaternion::Identity, nullptr);
            ASSERT_TRUE(body.IsValid()) << "body " << i;
            bodies.push_back(body);
        }
        EXPECT_FALSE(backend.CreateStaticBody(Vector3::Zero, Math::Quaternion::Identity, nullptr).IsValid());
        EXPECT_FALSE(backend.CreateDynamicBody(Vector3::Zero, Math::Quaternion::Identity, 1.0f, nullptr, false).IsValid());

        // A freed slot is usable again
        backend.DestroyBody(bodies[5]);
        const PhysicsBodyHandle again = backend.CreateStaticBody(Vector3::Zero, Math::Quaternion::Identity, nullptr);
        EXPECT_TRUE(again.IsValid());
        EXPECT_EQ(again.index, bodies[5].index);
        EXPECT_EQ(again.generation, bodies[5].generation + 1);
    }

    // ===== enable and disable =====

    TEST(JoltBackendTest, DisablingABodyKeepsItsIdAndItsStateOutOfTheSimulation)
    {
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        const PhysicsBodyHandle body =
            backend.CreateDynamicBody(Vector3(0.0f, 10.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
        backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
        const std::uint32_t id = backend.GetJoltBodyIndex(body);
        ASSERT_NE(id, UINT32_MAX);
        for (int i = 0; i < 10; ++i)
        {
            backend.Update(1.0f / 60.0f);
        }
        const float fallen = backend.GetPosition(body).y;
        ASSERT_LT(fallen, 10.0f);

        backend.SetBodyEnabled(body, false);
        EXPECT_FALSE(backend.IsBodyEnabled(body));
        BodyState state;
        EXPECT_FALSE(backend.GetBodyState(body, state)) << "a disabled body has no simulation state";
        EXPECT_FALSE(backend.SetBodyState(body, BodyState{}));
        backend.AddForce(body, Vector3(100.0f, 0.0f, 0.0f)); // no effect while out of the simulation
        for (int i = 0; i < 10; ++i)
        {
            backend.Update(1.0f / 60.0f);
        }
        EXPECT_FLOAT_EQ(backend.GetPosition(body).y, fallen) << "a disabled body kept falling";
        EXPECT_EQ(backend.GetJoltBodyIndex(body), id);

        backend.SetBodyEnabled(body, true);
        EXPECT_TRUE(backend.IsBodyEnabled(body));
        EXPECT_EQ(backend.GetJoltBodyIndex(body), id) << "the id changed with a disable and an enable";
        EXPECT_TRUE(backend.GetBodyState(body, state));
        for (int i = 0; i < 10; ++i)
        {
            backend.Update(1.0f / 60.0f);
        }
        EXPECT_LT(backend.GetPosition(body).y, fallen) << "an enabled body does not fall again";
        EXPECT_NEAR(backend.GetPosition(body).x, 0.0f, 1e-4f);
    }

    // ===== set-state =====

    TEST(JoltBackendTest, BodyStateRoundTripsForEveryKindOfBody)
    {
        JoltBackend backend;
        ASSERT_TRUE(backend.Initialize());
        const PhysicsMaterial material = PhysicsMaterial::Default();
        const Math::Quaternion identity = Math::Quaternion::Identity;

        const PhysicsBodyHandle dynamic = backend.CreateDynamicBody(Vector3(0.0f, 50.0f, 0.0f), identity, 1.0f, nullptr, false);
        backend.AddSphereCollider(dynamic, nullptr, 0.5f, Vector3::Zero, material);
        BodyState wanted;
        wanted.position = Vector3(1.0f, 20.0f, 3.0f);
        wanted.rotation = TurnedRotation();
        wanted.linearVelocity = Vector3(1.0f, 2.0f, -3.0f);
        wanted.angularVelocity = Vector3(0.5f, 0.0f, 0.25f);
        ASSERT_TRUE(backend.SetBodyState(dynamic, wanted));
        BodyState got;
        ASSERT_TRUE(backend.GetBodyState(dynamic, got));
        ExpectVectorNear(got.position, wanted.position, 1e-5f);
        ExpectVectorNear(got.linearVelocity, wanted.linearVelocity, 1e-5f);
        ExpectVectorNear(got.angularVelocity, wanted.angularVelocity, 1e-5f);
        EXPECT_NEAR(got.rotation.GetW(), wanted.rotation.GetW(), 1e-5f);
        EXPECT_NEAR(got.rotation.GetX(), wanted.rotation.GetX(), 1e-5f);
        EXPECT_FALSE(got.sleeping);

        // Asleep: no velocity, and it stays where it is until something wakes it
        BodyState asleep = wanted;
        asleep.sleeping = true;
        ASSERT_TRUE(backend.SetBodyState(dynamic, asleep));
        ASSERT_TRUE(backend.GetBodyState(dynamic, got));
        EXPECT_TRUE(got.sleeping);
        ExpectVectorNear(got.linearVelocity, Vector3::Zero, 0.0f);
        backend.Update(1.0f / 60.0f);
        ASSERT_TRUE(backend.GetBodyState(dynamic, got));
        EXPECT_NEAR(got.position.y, wanted.position.y, 1e-5f) << "a sleeping body moved";

        // Refused: nothing changes
        BodyState bad = wanted;
        bad.position.x = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(backend.SetBodyState(dynamic, bad));
        bad = wanted;
        bad.rotation = Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
        EXPECT_FALSE(backend.SetBodyState(dynamic, bad));
        BodyState after;
        ASSERT_TRUE(backend.GetBodyState(dynamic, after));
        ExpectVectorNear(after.position, got.position, 0.0f);

        // A static body takes a pose; its velocities are zero and it is never asleep
        const PhysicsBodyHandle fixed = backend.CreateStaticBody(Vector3::Zero, identity, nullptr);
        BodyState pose;
        pose.position = Vector3(5.0f, 0.0f, 0.0f);
        ASSERT_TRUE(backend.SetBodyState(fixed, pose));
        ASSERT_TRUE(backend.GetBodyState(fixed, got));
        ExpectVectorNear(got.position, Vector3(5.0f, 0.0f, 0.0f), 1e-6f);
        EXPECT_FALSE(got.sleeping);
        ExpectVectorNear(got.angularVelocity, Vector3::Zero, 0.0f);

        // A kinematic body takes a target: it is there after the next step
        const PhysicsBodyHandle mover = backend.CreateDynamicBody(Vector3::Zero, identity, 1.0f, nullptr, true);
        backend.AddBoxCollider(mover, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
        BodyState target;
        target.position = Vector3(0.0f, 0.0f, 4.0f);
        ASSERT_TRUE(backend.SetBodyState(mover, target));
        backend.Update(1.0f / 60.0f);
        ASSERT_TRUE(backend.GetBodyState(mover, got));
        EXPECT_NEAR(got.position.z, 4.0f, 1e-4f);
        ExpectVectorNear(got.linearVelocity, Vector3::Zero, 0.0f); // a kinematic body reports no velocity
    }

    // ===== scene fixture =====

    class JoltSceneTest : public PhysicsCoreFixture<JoltTag>
    {
    };

    TEST_F(JoltSceneTest, ATriggerColliderHasNoShapeYetAndWarnsOnce)
    {
        const unsigned before = Native().GetNotImplementedWarningCount();
        const auto go = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
        auto *solid = go->AddComponent<BoxCollider>();
        solid->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *trigger = go->AddComponent<SphereCollider>();
        trigger->SetRadius(0.5f);
        trigger->SetIsTrigger(true);
        auto *body = AddFloatingBody(*go, 1.0f);
        Step(1);

        EXPECT_EQ(Native().GetShapeCount(body->GetHandle()), 1u) << "the trigger's shape is left out until J4";
        const auto another = Spawn("Another", Vector3(5.0f, 0.0f, 0.0f));
        another->AddComponent<SphereCollider>()->SetIsTrigger(true);
        another->AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        Step(1);
        EXPECT_GE(Native().GetNotImplementedWarningCount(), 1u);
        EXPECT_LE(Native().GetNotImplementedWarningCount(), before + 1u) << "one warning for triggers, however many";
    }

    TEST_F(JoltSceneTest, EveryRigidbodyHasTheJoltIdOfItsHandle)
    {
        const auto first = SpawnBall("First", Vector3(0.0f, 5.0f, 0.0f));
        const auto second = SpawnBall("Second", Vector3(3.0f, 5.0f, 0.0f));
        Step(2);
        for (const auto &ball : {first, second})
        {
            const PhysicsBodyHandle handle = ball->template GetComponent<Rigidbody>()->GetHandle();
            ASSERT_TRUE(handle.IsValid());
            EXPECT_EQ(Native().GetJoltBodyIndex(handle), handle.index);
        }
    }

    // ===== determinism smoke =====

    TEST(JoltBackendDeterminismTest, TheSameSceneSteppedTwiceGivesTheSameStateHash)
    {
        const SceneResult first = RunScene(true);
        const SceneResult second = RunScene(true);
        PrintHash("single-threaded", first);

        EXPECT_TRUE(first.finite);
        EXPECT_TRUE(second.finite);
        EXPECT_EQ(first.bodies, second.bodies);
        EXPECT_EQ(first.hashAfter60, second.hashAfter60);
        EXPECT_EQ(first.hash, second.hash);
        // The scene did something: the state changed between step 60 and 240, the kinematic platform arrived
        EXPECT_NE(first.hashAfter60, first.hash);
        EXPECT_NEAR(first.platformX, -8.0f + 0.02f * 240.0f, 1e-3f);
        EXPECT_LT(first.ballHeight, 3.0f);
    }

    TEST(JoltBackendDeterminismTest, TheThreadPoolSceneIsRunAndItsHashPrinted)
    {
        // Printed, not compared: thread-count independence is proven in J6 (until then the Deterministic preset uses the
        // single-threaded job system). On the web this is the single-threaded scene again.
        const SceneResult pool = RunScene(false);
        PrintHash(kSingleThreadedBuild ? "single-threaded (web)" : "thread-pool", pool);
        EXPECT_TRUE(pool.finite);
        EXPECT_NEAR(pool.platformX, -8.0f + 0.02f * 240.0f, 1e-3f);
    }
}

#endif // N2ENGINE_JOLT_ENABLED
