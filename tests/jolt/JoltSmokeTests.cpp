// Smoke test of the vendored Jolt Physics library (external/JoltPhysics) alone: it links no engine target. Built and run
// on every CI leg that has N2ENGINE_USE_JOLT on, the web leg under node included (docs/testing.html).

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include "JoltScenario.hpp"

namespace
{

// What the build is, for the log line a CI step greps: compiler and architecture from the predefined macros
const char* BuildDescription()
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

unsigned long long AsPrintable(std::uint64_t value)
{
    return static_cast<unsigned long long>(value);
}

} // namespace

// (c) The build is the cross-platform deterministic one. The preprocessor definitions are checked when this file
// compiles (JoltScenario.hpp #errors on a wrong one); this checks what the library says it was built with.
TEST(JoltSmoke, BuildIsCrossPlatformDeterministic)
{
    const char* configuration = JPH::GetConfigurationString();
    ASSERT_NE(configuration, nullptr);
    std::printf("[JOLT] build: %s, configuration: %s\n", BuildDescription(), configuration);
    std::fflush(stdout);
    EXPECT_NE(std::strstr(configuration, "Cross Platform Deterministic"), nullptr) << configuration;
    EXPECT_EQ(std::strstr(configuration, "FMADD"), nullptr) << configuration;
    EXPECT_NE(std::strstr(configuration, "Single precision"), nullptr) << configuration;
}

// The translation units that include Jolt's headers get the same floating-point flags as the library (they are PUBLIC on
// Jolt::Jolt): a * b + c here must be two roundings. With fused multiply-add the result would be 2^-24 instead of 0.
TEST(JoltSmoke, NoFusedMultiplyAddInThisTranslationUnit)
{
    volatile float a = 1.0f + 1.0f / 4096.0f; // 1 + 2^-12
    volatile float b = 1.0f + 1.0f / 4096.0f;
    volatile float c = -(1.0f + 1.0f / 2048.0f); // -(1 + 2^-11)
    const float result = a * b + c;
    // a * b = 1 + 2^-11 + 2^-24 is half way between two floats; the round to even drops the 2^-24
    EXPECT_EQ(result, 0.0f);
}

// (a) A box dropped from 5 m with Jolt's default settings stops on the floor: its centre is half its height above the
// floor's top face (0.5), within Jolt's penetration slop (0.02), and it is not moving.
TEST(JoltSmoke, BoxComesToRestOnTheFloor)
{
    const n2jolt::RestResult rest = n2jolt::RunRestScenario();
    std::printf("[JOLT] rest: x=%.6f y=%.6f z=%.6f speed=%.6f\n", rest.x, rest.y, rest.z, rest.speed);
    std::fflush(stdout);
    EXPECT_TRUE(rest.stepsOk);
    EXPECT_NEAR(rest.y, 0.5f, 0.03f);
    EXPECT_NEAR(rest.x, 0.0f, 0.01f);
    EXPECT_NEAR(rest.z, 0.0f, 0.01f);
    EXPECT_LT(rest.speed, 0.05f);
}

// (b) The state after kDeterminismSteps steps hashes to one constant on every leg. Run twice in one process first:
// anything left over from the first run (global state, a counter) would show as a difference. The numbers are printed
// so a CI step shows them for every leg whether the comparison passed or not; do not loosen the comparison: a leg that
// differs is a finding about the library build on that leg.
TEST(JoltSmoke, StateHashEqualsTheGoldenOnEveryLeg)
{
    const n2jolt::DeterminismResult first = n2jolt::RunDeterminismScenario();
    const n2jolt::DeterminismResult second = n2jolt::RunDeterminismScenario();

    std::printf("[JOLT-DETERMINISM] build=%s bodies=%zu steps=%d hash60=0x%016llx hash240=0x%016llx hash=0x%016llx "
                "golden=0x%016llx\n",
                BuildDescription(), first.bodies, n2jolt::kDeterminismSteps, AsPrintable(first.hashAfter60),
                AsPrintable(first.hashAfter240), AsPrintable(first.hash), AsPrintable(n2jolt::kGoldenStateHash));
    std::fflush(stdout);

    EXPECT_TRUE(first.stepsOk);
    EXPECT_TRUE(first.finite);
    EXPECT_EQ(first.bodies, 6u);
    EXPECT_EQ(first.hash, second.hash);
    EXPECT_EQ(first.hashAfter60, second.hashAfter60);
    EXPECT_EQ(first.hashAfter240, second.hashAfter240);
    EXPECT_EQ(first.hash, n2jolt::kGoldenStateHash);
}
