#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <stdexcept>

#include <math/FloatEnvironment.hpp>

// The floating-point environment guard (netcode C1): the policy (round to nearest, denormals kept, no traps) is set for
// the life of the guard and what was there before is put back. The tests perturb the control register on purpose and prove
// the perturbation is real by what the arithmetic does, then prove the guard undoes it. A target with no control register
// (WebAssembly) has nothing to perturb: those tests are skipped there and the rest check the no-op contract.

using N2Engine::Math::FloatEnvironment;
using N2Engine::Math::FloatEnvironmentGuard;

namespace
{
    using Word = FloatEnvironment::Word;

    // 1/3 in float: the nearest float is 0x3EAAAAAB, rounding toward zero gives 0x3EAAAAAA. Read through volatile so no
    // compiler folds the division away at compile time under the default rounding mode.
    std::uint32_t OneThirdBits()
    {
        volatile float one = 1.0f;
        volatile float three = 3.0f;
        const float result = one / three;
        return std::bit_cast<std::uint32_t>(result);
    }

    // 1e-40f is a denormal float: with denormals-are-zero or flush-to-zero set, multiplying it by 1 gives 0
    std::uint32_t DenormalTimesOneBits()
    {
        volatile float denormal = 1.0e-40f;
        volatile float one = 1.0f;
        const float result = denormal * one;
        return std::bit_cast<std::uint32_t>(result);
    }

    // Puts the register back however the test ended
    struct RegisterRestorer
    {
        Word saved = FloatEnvironment::Read();
        ~RegisterRestorer() { FloatEnvironment::Write(saved); }
    };

    // The register with rounding toward zero and flushing both on: not the policy on any target
    Word Perturbed(const Word current)
    {
        return current | FloatEnvironment::kRoundingMask | FloatEnvironment::kFlushMask;
    }
}

TEST(FloatEnvironmentTest, ThePolicyIsNearestRoundingNoFlushAndMaskedExceptions)
{
    const RegisterRestorer restorer;
    if (!FloatEnvironment::kHasControlState)
    {
        EXPECT_EQ(FloatEnvironment::Read(), Word{0});
        EXPECT_TRUE(FloatEnvironment::IsDeterministic());
        GTEST_SKIP() << "this target has no floating-point control register";
    }
    // The policy value has no rounding bits and no flush bits, and the governed bits include both
    EXPECT_EQ(FloatEnvironment::kPolicy & FloatEnvironment::kRoundingMask, Word{0});
    EXPECT_EQ(FloatEnvironment::kPolicy & FloatEnvironment::kFlushMask, Word{0});
    EXPECT_EQ(FloatEnvironment::kGovernedMask & FloatEnvironment::kRoundingMask, FloatEnvironment::kRoundingMask);
    EXPECT_EQ(FloatEnvironment::kGovernedMask & FloatEnvironment::kFlushMask, FloatEnvironment::kFlushMask);
    // WithPolicy changes the governed bits and nothing else
    const Word everything = ~Word{0};
    EXPECT_EQ(FloatEnvironment::WithPolicy(everything) & ~FloatEnvironment::kGovernedMask,
              everything & ~FloatEnvironment::kGovernedMask);
    EXPECT_EQ(FloatEnvironment::WithPolicy(everything) & FloatEnvironment::kGovernedMask, FloatEnvironment::kPolicy);
}

TEST(FloatEnvironmentTest, TheGuardSetsThePolicyOverAPerturbedRegisterAndPutsItBack)
{
    if (!FloatEnvironment::kHasControlState)
    {
        GTEST_SKIP() << "this target has no floating-point control register";
    }
    const RegisterRestorer restorer;
    const Word perturbed = Perturbed(restorer.saved);
    FloatEnvironment::Write(perturbed);

    // The perturbation is real: rounding toward zero changes 1/3, and flushing zeroes a denormal
    ASSERT_FALSE(FloatEnvironment::IsDeterministic());
    ASSERT_EQ(OneThirdBits(), 0x3EAAAAAAu) << "rounding toward zero was not in effect";
    ASSERT_EQ(DenormalTimesOneBits(), 0u) << "flushing was not in effect";

    {
        const FloatEnvironmentGuard guard;
        EXPECT_TRUE(guard.Verified());
        EXPECT_EQ(guard.Previous(), perturbed);
        EXPECT_TRUE(FloatEnvironment::IsDeterministic());
        EXPECT_EQ(OneThirdBits(), 0x3EAAAAABu) << "the guard did not restore round-to-nearest";
        EXPECT_NE(DenormalTimesOneBits(), 0u) << "the guard did not turn flushing off";
    }

    // The scope ended: the perturbed state is back
    EXPECT_EQ(FloatEnvironment::Read() & FloatEnvironment::kGovernedMask, perturbed & FloatEnvironment::kGovernedMask);
    EXPECT_EQ(OneThirdBits(), 0x3EAAAAAAu);
    EXPECT_EQ(DenormalTimesOneBits(), 0u);
}

TEST(FloatEnvironmentTest, TheGuardRestoresTheRegisterWhenTheScopeIsLeftByAnException)
{
    if (!FloatEnvironment::kHasControlState)
    {
        GTEST_SKIP() << "this target has no floating-point control register";
    }
    const RegisterRestorer restorer;
    const Word perturbed = Perturbed(restorer.saved);
    FloatEnvironment::Write(perturbed);

    try
    {
        const FloatEnvironmentGuard guard;
        ASSERT_TRUE(FloatEnvironment::IsDeterministic());
        throw std::runtime_error("a tick failed");
    }
    catch (const std::runtime_error &)
    {
    }
    EXPECT_EQ(FloatEnvironment::Read() & FloatEnvironment::kGovernedMask, perturbed & FloatEnvironment::kGovernedMask);
    EXPECT_FALSE(FloatEnvironment::IsDeterministic());
}

TEST(FloatEnvironmentTest, GuardsNestAndEachPutsBackWhatItFound)
{
    if (!FloatEnvironment::kHasControlState)
    {
        GTEST_SKIP() << "this target has no floating-point control register";
    }
    const RegisterRestorer restorer;
    const Word perturbed = Perturbed(restorer.saved);
    FloatEnvironment::Write(perturbed);
    {
        const FloatEnvironmentGuard outer;
        EXPECT_TRUE(FloatEnvironment::IsDeterministic());
        {
            const FloatEnvironmentGuard inner;
            EXPECT_EQ(inner.Previous() & FloatEnvironment::kGovernedMask, FloatEnvironment::kPolicy);
            EXPECT_TRUE(FloatEnvironment::IsDeterministic());
        }
        EXPECT_TRUE(FloatEnvironment::IsDeterministic()) << "the inner guard put back the outer's policy";
    }
    EXPECT_EQ(FloatEnvironment::Read() & FloatEnvironment::kGovernedMask, perturbed & FloatEnvironment::kGovernedMask);
}

TEST(FloatEnvironmentTest, AGuardOverTheEnvironmentAlreadyAtThePolicyChangesNothing)
{
    const RegisterRestorer restorer;
    FloatEnvironment::Write(FloatEnvironment::WithPolicy(restorer.saved));
    const Word before = FloatEnvironment::Read();
    {
        const FloatEnvironmentGuard guard;
        EXPECT_TRUE(guard.Verified());
        EXPECT_TRUE(FloatEnvironment::IsDeterministic());
        EXPECT_EQ(OneThirdBits(), 0x3EAAAAABu);
    }
    EXPECT_EQ(FloatEnvironment::Read() & FloatEnvironment::kGovernedMask, before & FloatEnvironment::kGovernedMask);
}

// The arithmetic the deterministic math relies on, under the policy: round to nearest even and denormals kept
TEST(FloatEnvironmentTest, UnderTheGuardArithmeticIsRoundToNearestEvenWithDenormals)
{
    const RegisterRestorer restorer;
    if (FloatEnvironment::kHasControlState)
    {
        FloatEnvironment::Write(Perturbed(restorer.saved));
    }
    const FloatEnvironmentGuard guard;
    EXPECT_TRUE(guard.Verified());
    EXPECT_EQ(OneThirdBits(), 0x3EAAAAABu);
    EXPECT_NE(DenormalTimesOneBits(), 0u);
    // Ties to even: 1 + 2^-24 is exactly halfway between 1 and 1 + 2^-23 and rounds to the even one (1)
    volatile float one = 1.0f;
    volatile float half = 5.9604644775390625e-08f; // 2^-24
    const float sum = one + half;
    EXPECT_EQ(std::bit_cast<std::uint32_t>(sum), 0x3F800000u);
}
