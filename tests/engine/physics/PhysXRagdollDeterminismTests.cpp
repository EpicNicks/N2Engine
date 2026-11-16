#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// The ragdoll scene of the #157 1-ulp divergence experiment (PhysXDeterminismTests.cpp has free fall, box stack and
// pile; the engine's PhysX backend has no joints, so the ragdoll is built on the raw PhysX API with the same scene
// settings as the backend: PCM, stabilization, enhanced determinism, CPU dispatcher with 0 workers, dt = 1/60).
//
// ASSERTED: the same binary, two fresh scenes, the same ragdoll: every step bit-identical. PRINTED (never
// asserted; "[DETERMINISM]" lines): where a +1 ulp nudge of one initial coordinate separates the runs. See
// orchestrator/proposals/physx-determinism-157.md.
//
// A build without PhysX has nothing to run.

#if defined(N2ENGINE_PHYSX_ENABLED)

#include <PxPhysicsAPI.h>

using namespace physx;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;
    constexpr int kSteps = 600;

    class Fnv
    {
    public:
        void Add(const float f)
        {
            const auto bits = std::bit_cast<std::uint32_t>(f);
            for (int i = 0; i < 4; ++i)
            {
                _h ^= (bits >> (8 * i)) & 0xFFu;
                _h *= 0x100000001b3ull;
            }
        }
        std::uint64_t Value() const { return _h; }

    private:
        std::uint64_t _h = 0xcbf29ce484222325ull;
    };

    struct Trace
    {
        std::vector<std::uint64_t> stepHash;
        std::vector<std::vector<PxVec3>> pos; // [step][link]
        std::uint64_t final = 0;
    };

    // Links: 0 pelvis, 1 torso, 2 head, 3-4 upper arms, 5-6 forearms, 7-8 thighs, 9-10 shins
    struct Link
    {
        PxVec3 centre;
        PxVec3 halfExtents; // capsule: x = radius, y = half height (along local X after the 90 degree turn is not needed: boxes)
        int parent;
        PxVec3 jointFromParent; // joint position in world space at start
    };

    Trace SimulateRagdoll(const int perturbLink)
    {
        struct ErrorCb : PxErrorCallback
        {
            void reportError(PxErrorCode::Enum, const char *message, const char *, int) override
            {
                std::printf("PhysX: %s\n", message);
            }
        } errorCb;
        PxDefaultAllocator allocator;

        PxFoundation *foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errorCb);
        PxPhysics *physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), true, nullptr);
        PxInitExtensions(*physics, nullptr);
        PxDefaultCpuDispatcher *dispatcher = PxDefaultCpuDispatcherCreate(0);
        PxSceneDesc desc(physics->getTolerancesScale());
        desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
        desc.cpuDispatcher = dispatcher;
        desc.filterShader = PxDefaultSimulationFilterShader;
        desc.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_STABILIZATION | PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
        PxScene *scene = physics->createScene(desc);
        PxMaterial *material = physics->createMaterial(0.5f, 0.5f, 0.0f);

        PxRigidStatic *ground = PxCreatePlane(*physics, PxPlane(0, 1, 0, 0), *material);
        scene->addActor(*ground);

        const float y0 = 3.0f; // pelvis height; the tilt makes the landing asymmetric
        const Link links[] = {
            {{0.0f, y0, 0.0f}, {0.20f, 0.10f, 0.12f}, -1, {}},
            {{0.0f, y0 + 0.35f, 0.0f}, {0.20f, 0.25f, 0.12f}, 0, {0.0f, y0 + 0.10f, 0.0f}},
            {{0.0f, y0 + 0.85f, 0.0f}, {0.11f, 0.11f, 0.11f}, 1, {0.0f, y0 + 0.60f, 0.0f}},
            {{-0.40f, y0 + 0.55f, 0.0f}, {0.15f, 0.06f, 0.06f}, 1, {-0.25f, y0 + 0.55f, 0.0f}},
            {{0.40f, y0 + 0.55f, 0.0f}, {0.15f, 0.06f, 0.06f}, 1, {0.25f, y0 + 0.55f, 0.0f}},
            {{-0.70f, y0 + 0.55f, 0.0f}, {0.15f, 0.05f, 0.05f}, 3, {-0.55f, y0 + 0.55f, 0.0f}},
            {{0.70f, y0 + 0.55f, 0.0f}, {0.15f, 0.05f, 0.05f}, 4, {0.55f, y0 + 0.55f, 0.0f}},
            {{-0.12f, y0 - 0.35f, 0.0f}, {0.08f, 0.25f, 0.08f}, 0, {-0.12f, y0 - 0.10f, 0.0f}},
            {{0.12f, y0 - 0.35f, 0.0f}, {0.08f, 0.25f, 0.08f}, 0, {0.12f, y0 - 0.10f, 0.0f}},
            {{-0.12f, y0 - 0.85f, 0.0f}, {0.07f, 0.25f, 0.07f}, 7, {-0.12f, y0 - 0.60f, 0.0f}},
            {{0.12f, y0 - 0.85f, 0.0f}, {0.07f, 0.25f, 0.07f}, 8, {0.12f, y0 - 0.60f, 0.0f}},
        };
        constexpr int kLinks = static_cast<int>(sizeof(links) / sizeof(links[0]));

        std::vector<PxRigidDynamic *> bodies;
        for (int i = 0; i < kLinks; ++i)
        {
            PxVec3 p = links[i].centre;
            if (i == perturbLink)
            {
                // +1 ulp on the largest-magnitude coordinate
                float *c = &p.x;
                int axis = 0;
                for (int a = 1; a < 3; ++a)
                {
                    if (std::fabs((&p.x)[a]) > std::fabs(c[axis]))
                    {
                        axis = a;
                    }
                }
                c[axis] = std::nextafter(c[axis], std::numeric_limits<float>::infinity());
            }
            PxRigidDynamic *body = physics->createRigidDynamic(PxTransform(p));
            PxShape *shape = physics->createShape(PxBoxGeometry(links[i].halfExtents), *material);
            body->attachShape(*shape);
            shape->release();
            PxRigidBodyExt::updateMassAndInertia(*body, 100.0f);
            scene->addActor(*body);
            bodies.push_back(body);
        }
        // a sideways push and a spin so the ragdoll lands asymmetrically and tumbles
        bodies[0]->setLinearVelocity(PxVec3(1.5f, 0.0f, 0.7f));
        bodies[0]->setAngularVelocity(PxVec3(0.8f, 0.3f, -0.5f));

        for (int i = 1; i < kLinks; ++i)
        {
            const PxTransform parentFrame = bodies[links[i].parent]->getGlobalPose().getInverse() * PxTransform(links[i].jointFromParent);
            const PxTransform childFrame = bodies[i]->getGlobalPose().getInverse() * PxTransform(links[i].jointFromParent);
            PxSphericalJoint *joint = PxSphericalJointCreate(*physics, bodies[links[i].parent], parentFrame, bodies[i], childFrame);
            joint->setLimitCone(PxJointLimitCone(PxPi / 3.0f, PxPi / 3.0f));
            joint->setSphericalJointFlag(PxSphericalJointFlag::eLIMIT_ENABLED, true);
        }

        Trace trace;
        for (int step = 0; step < kSteps; ++step)
        {
            scene->simulate(kDt);
            scene->fetchResults(true);
            Fnv h;
            std::vector<PxVec3> positions;
            for (PxRigidDynamic *b : bodies)
            {
                const PxTransform t = b->getGlobalPose();
                const PxVec3 lv = b->getLinearVelocity(), av = b->getAngularVelocity();
                for (const float f : {t.p.x, t.p.y, t.p.z, t.q.w, t.q.x, t.q.y, t.q.z, lv.x, lv.y, lv.z, av.x, av.y, av.z})
                {
                    h.Add(f);
                }
                positions.push_back(t.p);
            }
            trace.stepHash.push_back(h.Value());
            trace.pos.push_back(std::move(positions));
        }
        trace.final = trace.stepHash.back();

        scene->release();
        dispatcher->release();
        PxCloseExtensions();
        physics->release();
        foundation->release();
        return trace;
    }

    std::string Hex(const std::uint64_t v)
    {
        char b[19];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
        return b;
    }

    std::string StepText(const int step)
    {
        return step == 0 ? "never" : "step " + std::to_string(step) + " (" + std::to_string(static_cast<float>(step) * kDt) + " s)";
    }
}

TEST(PhysXRagdollDeterminismTest, TwoFreshScenesEndBitIdentical)
{
    const Trace a = SimulateRagdoll(-1);
    const Trace b = SimulateRagdoll(-1);
    ASSERT_EQ(a.stepHash.size(), static_cast<std::size_t>(kSteps));
    EXPECT_EQ(a.stepHash, b.stepHash);
    std::printf("[DETERMINISM] scene=ragdoll (11 linked boxes) final-state-hash=%s\n", Hex(a.final).c_str());
}

TEST(PhysXRagdollDeterminismReportTest, OneUlpDivergence)
{
    const Trace base = SimulateRagdoll(-1);
    for (const int link : {0, 5})
    {
        const Trace p = SimulateRagdoll(link);
        int bitIdentical = kSteps, um = 0, mm = 0, cm = 0;
        float maxDist = 0.0f;
        for (int s = 0; s < kSteps; ++s)
        {
            if (bitIdentical == kSteps && base.stepHash[s] != p.stepHash[s])
            {
                bitIdentical = s;
            }
            float stepMax = 0.0f;
            for (std::size_t i = 0; i < base.pos[s].size(); ++i)
            {
                stepMax = std::max(stepMax, (base.pos[s][i] - p.pos[s][i]).magnitude());
            }
            if (!um && stepMax > 1e-6f) um = s + 1;
            if (!mm && stepMax > 1e-3f) mm = s + 1;
            if (!cm && stepMax > 1e-2f) cm = s + 1;
            maxDist = std::max(maxDist, stepMax);
        }
        std::printf("[DETERMINISM] scene=ragdoll (11 linked boxes) perturbed-link=%d bit-identical-steps=%d first>1um=%s first>1mm=%s first>1cm=%s max-divergence-m=%f final-state-hash=%s\n",
                    link, bitIdentical, StepText(um).c_str(), StepText(mm).c_str(), StepText(cm).c_str(), maxDist, Hex(p.final).c_str());
        std::fflush(stdout);
    }
}

// ===== Rollback by set-state (what PhysX offers instead of a snapshot) =====
// PhysX has no scene save/restore. The only restore the API allows is to write every body's pose, velocities and sleep
// state back (what IPhysicsBackend::SetBodyState does). This test restores a 60-body pile from step 150 into the same
// scene, runs 150 steps again and compares with the scene that ran straight through; contact caches, warm-start
// impulses and solver state are not restored, so a bit-exact replay is NOT expected (printed, never asserted). It
// also prints the cost of capturing and writing back the state. Same-binary, same-process.
namespace
{
    struct BodyState
    {
        PxTransform pose;
        PxVec3 lv, av;
        bool asleep;
    };

    std::uint64_t HashBodies(const std::vector<PxRigidDynamic *> &bodies)
    {
        Fnv h;
        for (PxRigidDynamic *b : bodies)
        {
            const PxTransform t = b->getGlobalPose();
            const PxVec3 lv = b->getLinearVelocity(), av = b->getAngularVelocity();
            for (const float f : {t.p.x, t.p.y, t.p.z, t.q.w, t.q.x, t.q.y, t.q.z, lv.x, lv.y, lv.z, av.x, av.y, av.z})
            {
                h.Add(f);
            }
        }
        return h.Value();
    }
}

TEST(PhysXRollbackSetStateReportTest, RestoreFidelityAndCost)
{
    struct ErrorCb : PxErrorCallback
    {
        void reportError(PxErrorCode::Enum, const char *, const char *, int) override {}
    } errorCb;
    PxDefaultAllocator allocator;
    PxFoundation *foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errorCb);
    PxPhysics *physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), true, nullptr);
    PxDefaultCpuDispatcher *dispatcher = PxDefaultCpuDispatcherCreate(0);
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_STABILIZATION | PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    PxScene *scene = physics->createScene(desc);
    PxMaterial *material = physics->createMaterial(0.5f, 0.5f, 0.0f);
    scene->addActor(*PxCreatePlane(*physics, PxPlane(0, 1, 0, 0), *material));

    std::vector<PxRigidDynamic *> bodies;
    std::uint64_t lcg = 12345;
    const auto next = [&lcg]()
    {
        lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((lcg >> 40) & 0xFFFFFFu) / 16777216.0f;
    };
    for (int i = 0; i < 60; ++i)
    {
        const float jx = next(), jy = next(), jz = next();
        const PxVec3 p(1.1f * static_cast<float>(i % 5 - 2) + 0.3f * (jx - 0.5f), 0.6f + 1.2f * static_cast<float>(i / 25) + 0.2f * jy,
                       1.1f * static_cast<float>((i / 5) % 5 - 2) + 0.3f * (jz - 0.5f));
        PxRigidDynamic *body = physics->createRigidDynamic(PxTransform(p));
        PxShape *shape = i % 2 ? physics->createShape(PxBoxGeometry(0.5f, 0.5f, 0.5f), *material)
                               : physics->createShape(PxSphereGeometry(0.5f), *material);
        body->attachShape(*shape);
        shape->release();
        PxRigidBodyExt::updateMassAndInertia(*body, 1.0f);
        scene->addActor(*body);
        bodies.push_back(body);
    }
    const auto step = [&](const int n)
    {
        for (int i = 0; i < n; ++i)
        {
            scene->simulate(kDt);
            scene->fetchResults(true);
        }
    };
    const auto capture = [&]()
    {
        std::vector<BodyState> out;
        out.reserve(bodies.size());
        for (PxRigidDynamic *b : bodies)
        {
            out.push_back({b->getGlobalPose(), b->getLinearVelocity(), b->getAngularVelocity(), b->isSleeping()});
        }
        return out;
    };
    const auto positions = [&]()
    {
        std::vector<PxVec3> out;
        for (PxRigidDynamic *b : bodies)
        {
            out.push_back(b->getGlobalPose().p);
        }
        return out;
    };

    step(150);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<BodyState> saved = capture();
    const auto t1 = std::chrono::steady_clock::now();
    step(150);
    const std::uint64_t straight = HashBodies(bodies);
    const std::vector<PxVec3> straightPos = positions();

    const auto t2 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        bodies[i]->setGlobalPose(saved[i].pose);
        bodies[i]->setLinearVelocity(saved[i].lv);
        bodies[i]->setAngularVelocity(saved[i].av);
        if (saved[i].asleep)
        {
            bodies[i]->putToSleep();
        }
        else
        {
            bodies[i]->wakeUp();
        }
    }
    const auto t3 = std::chrono::steady_clock::now();
    const std::uint64_t restoredHash = HashBodies(bodies);
    step(150);
    const std::uint64_t replay = HashBodies(bodies);
    const std::vector<PxVec3> replayPos = positions();
    float maxDist = 0.0f;
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        maxDist = std::max(maxDist, (straightPos[i] - replayPos[i]).magnitude());
    }
    const auto us = [](const auto a, const auto b)
    { return std::chrono::duration<double, std::micro>(b - a).count(); };
    std::printf("[DETERMINISM] scene=pile (60 mixed bodies) set-state rollback: restore-to-step-150 then 150 steps: replay %s the straight run (max position difference %f m); save %.1f us, restore %.1f us for 60 bodies (debug build, one sample)\n",
                replay == straight ? "EQUALS" : "DIFFERS FROM", maxDist, us(t0, t1), us(t2, t3));
    (void)restoredHash;
    std::fflush(stdout);

    scene->release();
    dispatcher->release();
    physics->release();
    foundation->release();
}

#else

TEST(PhysXRagdollDeterminismTest, NeedsPhysX)
{
    GTEST_SKIP() << "built without PhysX (N2ENGINE_USE_PHYSX=OFF)";
}

#endif
