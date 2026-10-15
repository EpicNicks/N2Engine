#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include <nlohmann/json.hpp>

#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"

// Determinism stage S1 (issue #157). Two kinds of result, kept apart on purpose:
//
//  - ASSERTED: the option's wiring (flag, worker threads, GPU refusal, default untouched) and that the SAME binary,
//    given the same scene twice in two fresh PhysX scenes with physics.deterministic on, ends in a bit-identical
//    state (positions, rotations, linear and angular velocities, every step). Nothing promises this across
//    platforms, so nothing here compares platforms.
//  - PRINTED, never asserted: the first step at which a 1 ulp perturbation of one initial position separates the
//    runs, the hash of the final state (to compare Windows and Linux by hand), and what changing the CPU worker
//    thread count or the enhanced-determinism flag does (the vendor headers promise neither). Lines start with
//    "[DETERMINISM]". ctest hides the output of a passing test, so run engine_tests directly (or ctest -V) and
//    look at that prefix (CI's "PhysX determinism numbers" step does, with --gtest_filter='*PhysXDeterminism*');
//    the lines are also appended to $GITHUB_STEP_SUMMARY when it is set.
//
// A build without PhysX (N2ENGINE_USE_PHYSX=OFF) has nothing to run.

// ===== The physics.deterministic setting (every build) =====

TEST(PhysicsDeterministicSettingTest, DefaultsToFalse)
{
    using json = nlohmann::json;
    std::vector<std::string> problems;
    EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json::object(), &problems));
    EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json::array(), &problems));
    EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json{{"physics", json::object()}}, &problems));
    EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json{{"physics", {{"deterministic", nullptr}}}}, &problems));
    EXPECT_TRUE(problems.empty());
}

TEST(PhysicsDeterministicSettingTest, ReadsABoolean)
{
    using json = nlohmann::json;
    std::vector<std::string> problems;
    EXPECT_TRUE(N2Engine::ReadPhysicsDeterministic(json{{"physics", {{"deterministic", true}}}}, &problems));
    EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json{{"physics", {{"deterministic", false}}}}, &problems));
    EXPECT_TRUE(problems.empty());
}

TEST(PhysicsDeterministicSettingTest, RefusesAnythingElseAndMeansFalse)
{
    using json = nlohmann::json;
    for (const json &bad : {json("true"), json(1), json(0), json::array(), json::object()})
    {
        std::vector<std::string> problems;
        EXPECT_FALSE(N2Engine::ReadPhysicsDeterministic(json{{"physics", {{"deterministic", bad}}}}, &problems))
            << bad.dump();
        EXPECT_EQ(problems.size(), 1u) << bad.dump();
    }
}

TEST(PhysicsDeterministicSettingTest, ApplyProjectSettingsValidatesIt)
{
    using json = nlohmann::json;
    EXPECT_TRUE(N2Engine::ApplyProjectSettings(json{{"physics", {{"deterministic", true}}}}, std::set<std::string>{"physics"}).empty());
    const auto problems = N2Engine::ApplyProjectSettings(json{{"physics", {{"deterministic", "yes"}}}}, std::set<std::string>{"physics"});
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems.front().find("physics.deterministic"), std::string::npos);
}

#ifdef N2ENGINE_PHYSX_ENABLED

// No `using namespace N2Engine`: math/MathRegistrar.hpp (included by ProjectSettings.hpp) forward-declares
// N2Engine::Vector3 and N2Engine::Quaternion, which would make the Math types below ambiguous
using namespace N2Engine::Physics;
using N2Engine::Logger;
using N2Engine::Math::Quaternion;
using N2Engine::Math::Vector3;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;
    constexpr int kSteps = 600; // 10 s

    // The 64-bit FNV-1a hash over the exact bit patterns of the floats: not a tolerance, a bit-exact comparison
    // (so -0 and +0, and a NaN payload, all count as different)
    class BitHasher
    {
    public:
        void Add(const float value)
        {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (int i = 0; i < 4; ++i)
            {
                _hash ^= static_cast<std::uint64_t>((bits >> (8 * i)) & 0xFFu);
                _hash *= 0x100000001b3ull;
            }
        }
        void Add(const std::uint64_t value)
        {
            for (int i = 0; i < 8; ++i)
            {
                _hash ^= (value >> (8 * i)) & 0xFFull;
                _hash *= 0x100000001b3ull;
            }
        }
        [[nodiscard]] std::uint64_t Value() const { return _hash; }

    private:
        std::uint64_t _hash = 0xcbf29ce484222325ull;
    };

    std::string Hex(const std::uint64_t value)
    {
        char buffer[19];
        std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
        return buffer;
    }

    // One printed line: stdout (flushed) and, in GitHub Actions, the job summary
    void Report(const std::string &line)
    {
        const std::string text = "[DETERMINISM] " + line;
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
        if (const char *summary = std::getenv("GITHUB_STEP_SUMMARY"); summary && *summary)
        {
            try
            {
                std::ofstream out(summary, std::ios::app);
                out << text << "  \n";
            }
            catch (...)
            {
                // the summary is a convenience
            }
        }
    }

    enum class SceneKind { FreeFall, BoxStack, Pile };

    const char *Name(const SceneKind scene)
    {
        switch (scene)
        {
        case SceneKind::FreeFall: return "free fall (1 sphere)";
        case SceneKind::BoxStack: return "box stack (10 boxes)";
        case SceneKind::Pile: return "pile (100 mixed bodies)";
        }
        return "?";
    }

    // A small integer generator, so scene layouts never depend on the engine's Random or on std distributions
    class Lcg
    {
    public:
        // In [0, 1)
        float Next()
        {
            _state = _state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>((_state >> 40) & 0xFFFFFFu) / 16777216.0f;
        }

    private:
        std::uint64_t _state = 12345;
    };

    struct Options
    {
        bool deterministic = true;
        std::optional<unsigned> workerThreads; // nullopt: whatever the option implies
        int perturbBody = -1; // index into the dynamic bodies: +1 ulp on its largest-magnitude initial coordinate
    };

    struct SimRun
    {
        std::uint64_t finalHash = 0;      // the state after the last step
        std::uint64_t trajectoryHash = 0; // every step's state folded in
        std::vector<std::uint64_t> stepHashes;       // [step], the full state hash after each step
        std::vector<std::vector<Vector3>> positions; // [step][body], after each step
        std::vector<Vector3> initialPositions;       // [body], read back from PhysX before the first step
        int perturbedAxis = -1;                      // 0 = x, 1 = y, 2 = z; -1 = nothing perturbed
        bool deterministicFlag = false;
        unsigned workers = 0;
    };

    // The state hash: position, rotation (w, x, y, z), linear and angular velocity of every dynamic body, in
    // creation order
    void HashState(PhysXBackend &backend, const std::vector<PhysicsBodyHandle> &bodies, BitHasher &hasher)
    {
        for (const PhysicsBodyHandle body : bodies)
        {
            const Vector3 p = backend.GetPosition(body);
            const Quaternion q = backend.GetRotation(body);
            const Vector3 v = backend.GetVelocity(body);
            const Vector3 w = backend.GetAngularVelocity(body);
            for (const float f : {p.x, p.y, p.z, q.GetW(), q.GetX(), q.GetY(), q.GetZ(), v.x, v.y, v.z, w.x, w.y, w.z})
            {
                hasher.Add(f);
            }
        }
    }

    float &Component(Vector3 &v, const int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
    float Component(const Vector3 &v, const int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

    // The coordinate a 1 ulp nudge is applied to: the largest in magnitude (the first on a tie), so it is never
    // 0, where nextafter would only reach the denormal 1.4e-45 and change nothing
    int LargestAxis(const Vector3 &v)
    {
        int axis = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (std::fabs(Component(v, i)) > std::fabs(Component(v, axis)))
            {
                axis = i;
            }
        }
        return axis;
    }

    // Insertion order is part of the scene: ground first, then the dynamic bodies in a fixed order. perturbedAxis
    // receives the axis nudged on body perturbBody (-1 when none is)
    std::vector<PhysicsBodyHandle> Build(PhysXBackend &backend, const SceneKind scene, const int perturbBody,
                                         int &perturbedAxis)
    {
        perturbedAxis = -1;
        const PhysicsMaterial material{};
        const Quaternion identity;
        std::vector<PhysicsBodyHandle> bodies;

        const PhysicsBodyHandle ground = backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), identity, nullptr);
        backend.AddBoxCollider(ground, nullptr, Vector3(50.0f, 0.5f, 50.0f), Vector3::Zero, material);

        const auto addBody = [&](Vector3 position, const int kind, const Vector3 &velocity, const Vector3 &spin)
        {
            if (static_cast<int>(bodies.size()) == perturbBody)
            {
                perturbedAxis = LargestAxis(position);
                float &coordinate = Component(position, perturbedAxis);
                coordinate = std::nextafter(coordinate, std::numeric_limits<float>::infinity());
            }
            const PhysicsBodyHandle body = backend.CreateDynamicBody(position, identity, 1.0f, nullptr, false);
            switch (kind)
            {
            case 0: backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material); break;
            case 1: backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, material); break;
            default: backend.AddCapsuleCollider(body, nullptr, 0.3f, 1.2f, Vector3::Zero, material); break;
            }
            backend.SetVelocity(body, velocity);
            backend.SetAngularVelocity(body, spin);
            bodies.push_back(body);
        };

        switch (scene)
        {
        case SceneKind::FreeFall:
            addBody(Vector3(0.0f, 10.0f, 0.0f), 1, Vector3(1.0f, 0.0f, 0.5f), Vector3(0.0f, 0.0f, 0.0f));
            break;
        case SceneKind::BoxStack:
            for (int i = 0; i < 10; ++i)
            {
                // a small alternating offset, so the stack is not perfectly aligned
                addBody(Vector3(0.01f * static_cast<float>(i % 2 == 0 ? i : -i), 0.5f + 1.02f * static_cast<float>(i), 0.0f),
                        0, Vector3::Zero, Vector3::Zero);
            }
            break;
        case SceneKind::Pile:
        {
            Lcg lcg;
            for (int i = 0; i < 100; ++i)
            {
                const int column = i % 5;
                const int row = (i / 5) % 5;
                const int layer = i / 25;
                // Every draw goes into a named local, one statement each, in this order: the order function
                // arguments are evaluated in is unspecified (MSVC and Clang differ), so draws inside one
                // Vector3(...) call would give each compiler a different pile
                const float jitterX = lcg.Next();
                const float jitterY = lcg.Next();
                const float jitterZ = lcg.Next();
                const float spinX = lcg.Next();
                const float spinY = lcg.Next();
                const float spinZ = lcg.Next();
                const float fallSpeed = lcg.Next();
                const Vector3 position(1.3f * static_cast<float>(column - 2) + 0.4f * (jitterX - 0.5f),
                                       1.0f + 1.6f * static_cast<float>(layer) + 0.2f * jitterY,
                                       1.3f * static_cast<float>(row - 2) + 0.4f * (jitterZ - 0.5f));
                const Vector3 spin(spinX - 0.5f, spinY - 0.5f, spinZ - 0.5f);
                addBody(position, i % 3, Vector3(0.0f, -1.0f * fallSpeed, 0.0f), spin);
            }
            break;
        }
        }
        return bodies;
    }

    // A fresh backend (a fresh PhysX foundation, SDK and scene) for each run
    SimRun Simulate(const SceneKind scene, const Options &options)
    {
        SimRun run;
        PhysXBackend backend;
        backend.SetDeterministic(options.deterministic);
        backend.SetWorkerThreadsForTesting(options.workerThreads);
        EXPECT_TRUE(backend.Initialize());
        run.deterministicFlag = backend.SceneUsesEnhancedDeterminism();
        run.workers = backend.DispatcherWorkerCount();

        const std::vector<PhysicsBodyHandle> bodies = Build(backend, scene, options.perturbBody, run.perturbedAxis);
        for (const PhysicsBodyHandle body : bodies)
        {
            run.initialPositions.push_back(backend.GetPosition(body));
        }
        BitHasher trajectory;
        for (int step = 0; step < kSteps; ++step)
        {
            backend.Update(kDt);
            BitHasher stepHash;
            HashState(backend, bodies, stepHash);
            trajectory.Add(stepHash.Value());
            run.stepHashes.push_back(stepHash.Value());

            std::vector<Vector3> positions;
            positions.reserve(bodies.size());
            for (const PhysicsBodyHandle body : bodies)
            {
                positions.push_back(backend.GetPosition(body));
            }
            run.positions.push_back(std::move(positions));
        }
        BitHasher finalHasher;
        HashState(backend, bodies, finalHasher);
        run.finalHash = finalHasher.Value();
        run.trajectoryHash = trajectory.Value();
        backend.Shutdown();
        return run;
    }

    float Distance(const Vector3 &a, const Vector3 &b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct Divergence
    {
        // The first step (1-based) at which any body differs by more than the threshold; 0 = never
        int overMicrometre = 0, overMillimetre = 0, overCentimetre = 0;
        float maxDistance = 0.0f;
        // Steps before the first step whose full state hash (position, rotation, linear and angular velocity of
        // every body, the same hash the asserted test compares) differs
        int bitIdenticalSteps = 0;
    };

    Divergence Compare(const SimRun &a, const SimRun &b)
    {
        Divergence result;
        bool differs = false;
        for (std::size_t step = 0; step < a.positions.size(); ++step)
        {
            if (!differs && a.stepHashes[step] != b.stepHashes[step])
            {
                differs = true;
                result.bitIdenticalSteps = static_cast<int>(step);
            }
            float stepMax = 0.0f;
            for (std::size_t i = 0; i < a.positions[step].size(); ++i)
            {
                stepMax = std::max(stepMax, Distance(a.positions[step][i], b.positions[step][i]));
            }
            const int oneBased = static_cast<int>(step) + 1;
            if (!result.overMicrometre && stepMax > 1e-6f) result.overMicrometre = oneBased;
            if (!result.overMillimetre && stepMax > 1e-3f) result.overMillimetre = oneBased;
            if (!result.overCentimetre && stepMax > 1e-2f) result.overCentimetre = oneBased;
            result.maxDistance = std::max(result.maxDistance, stepMax);
        }
        if (!differs)
        {
            result.bitIdenticalSteps = static_cast<int>(a.positions.size());
        }
        return result;
    }

    std::string StepText(const int step)
    {
        return step == 0 ? "never" : "step " + std::to_string(step) + " (" + std::to_string(static_cast<float>(step) * kDt) + " s)";
    }

    // The Logger's warnings, while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };
}

// ===== The option (asserted) =====

TEST(PhysXDeterminismOptionTest, DefaultSceneIsUnchanged)
{
    PhysXBackend backend;
    ASSERT_TRUE(backend.Initialize());
    EXPECT_FALSE(backend.SceneUsesEnhancedDeterminism());
    EXPECT_EQ(backend.DispatcherWorkerCount(), 4u);
    EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled);
    backend.Shutdown();
}

TEST(PhysXDeterminismOptionTest, ExplicitlyOffIsTheDefaultScene)
{
    PhysXBackend backend;
    backend.SetDeterministic(false);
    ASSERT_TRUE(backend.Initialize());
    EXPECT_FALSE(backend.SceneUsesEnhancedDeterminism());
    EXPECT_EQ(backend.DispatcherWorkerCount(), 4u);
    backend.Shutdown();
}

TEST(PhysXDeterminismOptionTest, OnSetsEnhancedDeterminismAndZeroWorkerThreads)
{
    PhysXBackend backend;
    backend.SetDeterministic(true);
    ASSERT_TRUE(backend.Initialize());
    EXPECT_TRUE(backend.SceneUsesEnhancedDeterminism());
    EXPECT_EQ(backend.DispatcherWorkerCount(), 0u);
    EXPECT_FALSE(backend.SceneUsesGpuDynamics());
    backend.Shutdown();
}

TEST(PhysXDeterminismOptionTest, WithGpuPhysicsItWarnsAndBuildsTheCpuScene)
{
    // GPU dynamics does not support enhanced determinism: the CPU scene, no CUDA call, one clear warning
    for (const GpuMode mode : {GpuMode::Auto, GpuMode::On})
    {
        const WarningCapture capture;
        PhysXBackend backend;
        backend.SetGpuMode(mode);
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        EXPECT_TRUE(backend.SceneUsesEnhancedDeterminism()) << ToString(mode);
        EXPECT_EQ(backend.DispatcherWorkerCount(), 0u) << ToString(mode);
        EXPECT_FALSE(backend.SceneUsesGpuDynamics()) << ToString(mode);
        EXPECT_FALSE(backend.HasCudaContextManager()) << ToString(mode);
        EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled) << ToString(mode);
        EXPECT_NE(backend.GetGpuStatus().detail.find("physics.deterministic"), std::string::npos);
        int mentions = 0;
        for (const std::string &warning : capture.warnings)
        {
            mentions += warning.find("physics.deterministic") != std::string::npos ? 1 : 0;
        }
        EXPECT_EQ(mentions, 1) << ToString(mode);
        backend.Shutdown();
    }
}

TEST(PhysXDeterminismOptionTest, WithGpuOffThereIsNoWarning)
{
    const WarningCapture capture;
    PhysXBackend backend;
    backend.SetDeterministic(true);
    ASSERT_TRUE(backend.Initialize());
    for (const std::string &warning : capture.warnings)
    {
        EXPECT_EQ(warning.find("physics.deterministic"), std::string::npos) << warning;
    }
    backend.Shutdown();
}

// ===== Same-binary determinism (asserted) =====

class PhysXDeterminismRunTest : public ::testing::TestWithParam<SceneKind>
{
};

TEST_P(PhysXDeterminismRunTest, TwoFreshScenesEndBitIdentical)
{
    const SceneKind scene = GetParam();
    const SimRun first = Simulate(scene, {});
    const SimRun second = Simulate(scene, {});
    ASSERT_TRUE(first.deterministicFlag);
    ASSERT_TRUE(second.deterministicFlag);
    ASSERT_EQ(first.workers, 0u);
    ASSERT_EQ(first.positions.size(), static_cast<std::size_t>(kSteps));

    // The scene must actually have moved, or equal hashes prove nothing
    bool moved = false;
    for (std::size_t i = 0; i < first.positions.front().size(); ++i)
    {
        moved = moved || first.positions.front()[i].y != first.positions.back()[i].y;
    }
    EXPECT_TRUE(moved) << Name(scene);

    EXPECT_EQ(Hex(first.finalHash), Hex(second.finalHash)) << Name(scene);
    EXPECT_EQ(Hex(first.trajectoryHash), Hex(second.trajectoryHash)) << Name(scene);
    const Divergence divergence = Compare(first, second);
    EXPECT_EQ(divergence.bitIdenticalSteps, kSteps) << Name(scene);
    EXPECT_EQ(divergence.maxDistance, 0.0f) << Name(scene);

    // Not asserted: the value, to compare Windows and Linux (and Debug and Release) by hand
    Report(std::string("scene=") + Name(scene) + " steps=" + std::to_string(kSteps) +
           " final-state-hash=" + Hex(first.finalHash) + " trajectory-hash=" + Hex(first.trajectoryHash));
}

INSTANTIATE_TEST_SUITE_P(Scenes, PhysXDeterminismRunTest,
                         ::testing::Values(SceneKind::FreeFall, SceneKind::BoxStack, SceneKind::Pile),
                         [](const ::testing::TestParamInfo<SceneKind> &info)
                         {
                             switch (info.param)
                             {
                             case SceneKind::FreeFall: return std::string("FreeFall");
                             case SceneKind::BoxStack: return std::string("BoxStack");
                             case SceneKind::Pile: return std::string("Pile");
                             }
                             return std::string("Unknown");
                         });

// ===== Measurements (printed, never asserted) =====

// The divergence numbers for real PhysX (the report in the issue only has an illustration with a 2D model), the
// thread-count and flag experiments, and the final-state hashes. Only the plumbing is asserted.
TEST(PhysXDeterminismReportTest, OneUlpDivergenceAndOptionExperiments)
{
    Report(std::string("one-ulp experiment: dt=1/60, ") + std::to_string(kSteps) +
           " steps, enhanced determinism on, 0 worker threads; one body's largest-magnitude initial coordinate"
           " moved by +1 ulp; bit-identical-steps compares the full state hash (position, rotation, velocities)");
    for (const SceneKind scene : {SceneKind::FreeFall, SceneKind::BoxStack, SceneKind::Pile})
    {
        const SimRun baseline = Simulate(scene, {});
        ASSERT_EQ(baseline.positions.size(), static_cast<std::size_t>(kSteps));
        const int bodyCount = static_cast<int>(baseline.positions.front().size());

        // Perturb the first body, and (for more than one body) the one in the middle
        std::vector<int> perturbed{0};
        if (bodyCount > 1)
        {
            perturbed.push_back(bodyCount / 2);
        }
        for (const int body : perturbed)
        {
            Options options;
            options.perturbBody = body;
            const SimRun perturbedRun = Simulate(scene, options);

            // The nudge must be real: a non-zero coordinate, and PhysX must hold a bitwise different start value
            // for that body than in the baseline (so a no-op can never pass as a 1 ulp result again)
            ASSERT_EQ(baseline.perturbedAxis, -1) << Name(scene);
            const int axis = perturbedRun.perturbedAxis;
            ASSERT_GE(axis, 0) << Name(scene) << " body " << body;
            ASSERT_LT(axis, 3) << Name(scene) << " body " << body;
            const float original = Component(baseline.initialPositions[body], axis);
            const float nudged = Component(perturbedRun.initialPositions[body], axis);
            ASSERT_NE(original, 0.0f) << Name(scene) << " body " << body << " axis " << axis;
            ASSERT_TRUE(std::isnormal(original)) << Name(scene) << " body " << body << " axis " << axis;
            ASSERT_NE(std::bit_cast<std::uint32_t>(nudged), std::bit_cast<std::uint32_t>(original))
                << Name(scene) << " body " << body << " axis " << axis;
            ASSERT_EQ(std::bit_cast<std::uint32_t>(nudged),
                      std::bit_cast<std::uint32_t>(std::nextafter(original, std::numeric_limits<float>::infinity())))
                << Name(scene) << " body " << body << " axis " << axis;
            for (int other = 0; other < bodyCount; ++other)
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (other != body || i != axis)
                    {
                        ASSERT_EQ(std::bit_cast<std::uint32_t>(Component(perturbedRun.initialPositions[other], i)),
                                  std::bit_cast<std::uint32_t>(Component(baseline.initialPositions[other], i)))
                            << Name(scene) << " body " << other << " axis " << i;
                    }
                }
            }

            const Divergence d = Compare(baseline, perturbedRun);
            Report(std::string("scene=") + Name(scene) + " perturbed-body=" + std::to_string(body) +
                   " axis=" + std::string(1, "xyz"[axis]) + " start=" + std::to_string(original) +
                   " bit-identical-steps=" + std::to_string(d.bitIdenticalSteps) +
                   " first>1um=" + StepText(d.overMicrometre) + " first>1mm=" + StepText(d.overMillimetre) +
                   " first>1cm=" + StepText(d.overCentimetre) + " max-divergence-m=" + std::to_string(d.maxDistance) +
                   " final-state-hash=" + Hex(perturbedRun.finalHash));
        }

        // The option set to 4 worker threads (the flag stays on), and the flag off with the default 4 threads
        Options fourThreads;
        fourThreads.workerThreads = 4u;
        const SimRun withFour = Simulate(scene, fourThreads);
        const Divergence threads = Compare(baseline, withFour);
        Report(std::string("scene=") + Name(scene) + " threads 0 vs 4: " +
               (baseline.finalHash == withFour.finalHash && baseline.trajectoryHash == withFour.trajectoryHash
                    ? "bit-identical"
                    : "DIFFERENT (first>1mm=" + StepText(threads.overMillimetre) +
                          " max-divergence-m=" + std::to_string(threads.maxDistance) + ")") +
               " workers=" + std::to_string(withFour.workers) + " final-state-hash=" + Hex(withFour.finalHash));

        Options flagOff;
        flagOff.deterministic = false;
        const SimRun off1 = Simulate(scene, flagOff);
        const SimRun off2 = Simulate(scene, flagOff);
        Report(std::string("scene=") + Name(scene) + " option off (flag off, 4 threads): run vs run " +
               (off1.trajectoryHash == off2.trajectoryHash ? "bit-identical" : "DIFFERENT") +
               ", vs option on " + (off1.trajectoryHash == baseline.trajectoryHash ? "bit-identical" : "DIFFERENT") +
               " workers=" + std::to_string(off1.workers) + " final-state-hash=" + Hex(off1.finalHash));
    }
}

#else // !N2ENGINE_PHYSX_ENABLED

TEST(PhysXDeterminismTest, NeedsPhysX)
{
    GTEST_SKIP() << "built without PhysX (N2ENGINE_USE_PHYSX=OFF)";
}

#endif // N2ENGINE_PHYSX_ENABLED
