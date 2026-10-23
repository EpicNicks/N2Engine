// PhysX on the web (stage 5 of issue #155; docs/web-build.html, "PhysX on the web"): what is specific to the
// WebAssembly build of PhysX and not covered by the engine's own physics tests (those run unchanged under node:
// tests/engine/physics). Run under node like the other web tests.

#include <gtest/gtest.h>

#include <cstdio>
#include <malloc.h>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include "engine/Logger.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED

#include <PxPhysicsAPI.h>

#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/physx/PhysXLoggerErrorCallback.hpp"

using N2Engine::Logger;
using N2Engine::Physics::PhysXBackend;
using N2Engine::Physics::PhysXLoggerErrorCallback;

namespace
{
    // Every message the Logger gets while this object lives
    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                entries.emplace_back(level, std::string(message));
            };
        }
        ~LogCapture() { Logger::logEvent -= _id; }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] int Count(const Logger::LogLevel level, const std::string &text) const
        {
            int count = 0;
            for (const auto &[entryLevel, message] : entries)
            {
                count += (entryLevel == level && message.find(text) != std::string::npos) ? 1 : 0;
            }
            return count;
        }

        std::vector<std::pair<Logger::LogLevel, std::string>> entries;

    private:
        std::size_t _id = 0;
    };
}

// The libraries and the engine agree on the build: scalar math (the SIMD path needs WebAssembly SIMD), no threads
TEST(PhysXWebTest, BuiltForScalarSingleThreadedWebAssembly)
{
#ifndef PX_SIMD_DISABLED
    FAIL() << "the engine must define PX_SIMD_DISABLED like the PhysX libraries were built with";
#endif
#ifndef N2ENGINE_SINGLE_THREADED
    FAIL() << "the web build is single-threaded";
#endif
    EXPECT_EQ(PX_EMSCRIPTEN, 1);
    EXPECT_EQ(PX_SSE2, 0);
    EXPECT_EQ(PX_SUPPORT_GPU_PHYSX, 0); // no CUDA types: the GPU scene and the particle driver are not built
    EXPECT_EQ(sizeof(void *), 4u);
}

TEST(PhysXWebTest, ErrorCallbackMapsEveryCodeToALogLevel)
{
    using physx::PxErrorCode;
    EXPECT_EQ(PhysXLoggerErrorCallback::LevelOf(PxErrorCode::eNO_ERROR), Logger::LogLevel::Info);
    EXPECT_EQ(PhysXLoggerErrorCallback::LevelOf(PxErrorCode::eDEBUG_INFO), Logger::LogLevel::Info);
    EXPECT_EQ(PhysXLoggerErrorCallback::LevelOf(PxErrorCode::eDEBUG_WARNING), Logger::LogLevel::Warn);
    EXPECT_EQ(PhysXLoggerErrorCallback::LevelOf(PxErrorCode::ePERF_WARNING), Logger::LogLevel::Warn);
    for (const PxErrorCode::Enum code : {PxErrorCode::eINVALID_PARAMETER, PxErrorCode::eINVALID_OPERATION,
                                         PxErrorCode::eOUT_OF_MEMORY, PxErrorCode::eINTERNAL_ERROR,
                                         PxErrorCode::eABORT})
    {
        EXPECT_EQ(PhysXLoggerErrorCallback::LevelOf(code), Logger::LogLevel::Error) << static_cast<int>(code);
    }
}

TEST(PhysXWebTest, ErrorCallbackWritesTheMessageFileAndLineToTheLogger)
{
    const LogCapture capture;
    PhysXLoggerErrorCallback callback;
    callback.reportError(physx::PxErrorCode::eINVALID_PARAMETER, "radius must be positive", "Shape.cpp", 42);
    callback.reportError(physx::PxErrorCode::ePERF_WARNING, "many contacts", "Solver.cpp", 7);
    EXPECT_EQ(capture.Count(Logger::LogLevel::Error, "PhysX invalid parameter: radius must be positive (Shape.cpp:42)"), 1);
    EXPECT_EQ(capture.Count(Logger::LogLevel::Warn, "PhysX performance warning: many contacts (Solver.cpp:7)"), 1);
    EXPECT_EQ(PhysXLoggerErrorCallback::Describe(physx::PxErrorCode::eABORT, nullptr, nullptr, 3),
              "PhysX abort:  (?:3)");
}

// A real error from the SDK (an SDK created with the wrong version number reports eINVALID_PARAMETER and returns
// nothing) reaches the Logger through the callback that was given to PxCreateFoundation
TEST(PhysXWebTest, ErrorsReportedByTheSdkReachTheLogger)
{
    const LogCapture capture;
    static physx::PxDefaultAllocator allocator;
    PhysXLoggerErrorCallback callback;
    physx::PxFoundation *foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, callback);
    ASSERT_NE(foundation, nullptr);
    physx::PxPhysics *wrongVersion = PxCreatePhysics(PX_PHYSICS_VERSION + 1, *foundation, physx::PxTolerancesScale(),
                                                     false, nullptr);
    EXPECT_EQ(wrongVersion, nullptr);
    if (wrongVersion)
    {
        wrongVersion->release();
    }
    foundation->release();
    EXPECT_GE(capture.Count(Logger::LogLevel::Error, "PhysX invalid parameter"), 1);
}

// What a scene costs in wasm memory and time under node, printed and only loosely asserted (docs/web-build.html, "PhysX
// on the web"): the heap may grow (-sALLOW_MEMORY_GROWTH=1; no up-front allocation was added for PhysX), so this is the
// number to watch. The times are those of node, not of a browser, and are not a performance claim.
TEST(PhysXWebTest, ABoxStackCostsAModestHeapGrowthAndSteps)
{
    constexpr int kBoxes = 200;
    constexpr int kSteps = 120;
    const std::size_t heapBefore = emscripten_get_heap_size();
    const std::size_t usedBefore = static_cast<std::size_t>(mallinfo().uordblks);

    static physx::PxDefaultAllocator allocator;
    PhysXLoggerErrorCallback callback;
    physx::PxFoundation *foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, callback);
    ASSERT_NE(foundation, nullptr);
    physx::PxPhysics *physics =
        PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, physx::PxTolerancesScale(), true, nullptr);
    ASSERT_NE(physics, nullptr);
    physx::PxDefaultCpuDispatcher *dispatcher = physx::PxDefaultCpuDispatcherCreate(0);
    ASSERT_NE(dispatcher, nullptr);
    physx::PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = physx::PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = physx::PxDefaultSimulationFilterShader;
    desc.flags |= physx::PxSceneFlag::eENABLE_PCM | physx::PxSceneFlag::eENABLE_STABILIZATION;
    physx::PxScene *scene = physics->createScene(desc);
    ASSERT_NE(scene, nullptr);
    physx::PxMaterial *material = physics->createMaterial(0.5f, 0.5f, 0.1f);

    physx::PxRigidStatic *floor = physics->createRigidStatic(physx::PxTransform(physx::PxVec3(0.0f, -0.5f, 0.0f)));
    physx::PxRigidActorExt::createExclusiveShape(*floor, physx::PxBoxGeometry(50.0f, 0.5f, 50.0f), *material);
    scene->addActor(*floor);
    for (int i = 0; i < kBoxes; ++i)
    {
        const float x = static_cast<float>(i % 5) * 1.2f - 2.4f;
        const float y = 0.5f + static_cast<float>(i / 25) * 1.2f;
        const float z = static_cast<float>((i / 5) % 5) * 1.2f - 2.4f;
        physx::PxRigidDynamic *box = physics->createRigidDynamic(physx::PxTransform(physx::PxVec3(x, y, z)));
        physx::PxRigidActorExt::createExclusiveShape(*box, physx::PxBoxGeometry(0.5f, 0.5f, 0.5f), *material);
        physx::PxRigidBodyExt::updateMassAndInertia(*box, 1.0f);
        scene->addActor(*box);
    }

    const double start = emscripten_get_now();
    for (int i = 0; i < kSteps; ++i)
    {
        scene->simulate(1.0f / 60.0f);
        scene->fetchResults(true);
    }
    const double elapsedMs = emscripten_get_now() - start;
    const std::size_t heapAfter = emscripten_get_heap_size();
    const std::size_t usedAfter = static_cast<std::size_t>(mallinfo().uordblks);

    std::printf("[PHYSX-WEB] %d boxes on a floor, %d steps of 1/60 s, 0 workers, under node: %.1f ms in total (%.2f ms per step); "
                "wasm heap %zu -> %zu bytes (+%zu), malloc'ed bytes in use %zu -> %zu (+%zu)\n",
                kBoxes, kSteps, elapsedMs, elapsedMs / kSteps, heapBefore, heapAfter, heapAfter - heapBefore, usedBefore,
                usedAfter, usedAfter - usedBefore);
    EXPECT_LT(heapAfter - heapBefore, static_cast<std::size_t>(64) * 1024 * 1024);
    EXPECT_LT(usedAfter - usedBefore, static_cast<std::size_t>(64) * 1024 * 1024);

    scene->release();
    material->release();
    dispatcher->release();
    physics->release();
    foundation->release();
}

// The backend itself: it builds on the web, runs its scene with no worker threads and steps
TEST(PhysXWebTest, BackendStepsWithNoWorkerThreads)
{
    PhysXBackend backend;
    ASSERT_TRUE(backend.Initialize());
    EXPECT_EQ(backend.DispatcherWorkerCount(), 0u);
    for (int i = 0; i < 10; ++i)
    {
        backend.Update(1.0f / 60.0f);
    }
    backend.Shutdown();
}

#else // !N2ENGINE_PHYSX_ENABLED

TEST(PhysXWebTest, NeedsPhysX)
{
    GTEST_SKIP() << "built without PhysX (-DN2ENGINE_USE_PHYSX=OFF, or the PhysX libraries could not be built)";
}

#endif // N2ENGINE_PHYSX_ENABLED
