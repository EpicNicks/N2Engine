#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define N2_FP_CONTROL_X86 1
#elif defined(__aarch64__)
#define N2_FP_CONTROL_ARM64 1
#endif

namespace N2Engine::Math
{
    /**
     * The floating-point control state the deterministic simulation depends on, and the policy it must be in (netcode C1,
     * "P7" of the physics-backend investigation). Floating-point arithmetic is only the same on two machines when both
     * round the same way and treat denormals the same way, and a third-party library (an audio or graphics driver, a
     * plugin) can change those settings in the thread that calls it.
     *
     * The policy, the same on every platform:
     *   - rounding to nearest, ties to even;
     *   - denormals kept: flush-to-zero OFF, denormals-are-zero OFF (a game that wants them flushed would no longer be
     *     the same game on the platform that cannot flush);
     *   - every floating-point exception masked (no trap): an invalid operation gives a NaN, never a signal.
     *
     * What exists: x86-64 has the SSE control register MXCSR (read and written here; x87 is not used on x86-64, where
     * float and double arithmetic is SSE) and aarch64 has the FPCR. WebAssembly has no control state at all (it always
     * rounds to nearest, never flushes and never traps), and neither does a target this code does not know (32-bit x86,
     * MSVC on arm64): there kHasControlState is false, Read returns 0, Write does nothing and IsDeterministic is true.
     */
    class FloatEnvironment
    {
    public:
        using Word = std::uint64_t;

#if defined(N2_FP_CONTROL_X86)
        static constexpr bool kHasControlState = true;
        /// MXCSR bits: exception masks 7-12, rounding control 13-14, flush-to-zero 15, denormals-are-zero 6
        static constexpr Word kRoundingMask = 0x6000u;
        static constexpr Word kFlushMask = 0x8040u;
        static constexpr Word kGovernedMask = 0xFFC0u;
        /// All exceptions masked (0x1F80), rounding to nearest, no flushing
        static constexpr Word kPolicy = 0x1F80u;
#elif defined(N2_FP_CONTROL_ARM64)
        static constexpr bool kHasControlState = true;
        /// FPCR bits: RMode 22-23, FZ 24, FZ16 19, the exception trap enables IOE..IXE 8-12 and IDE 15, and the
        /// alternate-behaviour bits FIZ, AH, NEP 0-2
        static constexpr Word kRoundingMask = Word{3} << 22;
        static constexpr Word kFlushMask = (Word{1} << 24) | (Word{1} << 19);
        static constexpr Word kGovernedMask = kRoundingMask | kFlushMask | (Word{0x1F} << 8) | (Word{1} << 15) | Word{7};
        /// Everything clear: nearest, no flushing, no traps, the standard behaviour
        static constexpr Word kPolicy = 0;
#else
        static constexpr bool kHasControlState = false;
        static constexpr Word kRoundingMask = 0;
        static constexpr Word kFlushMask = 0;
        static constexpr Word kGovernedMask = 0;
        static constexpr Word kPolicy = 0;
#endif

        /// The control register as it is now (all of it, including the bits the policy does not govern); 0 without one
        [[nodiscard]] static Word Read();
        /// Writes the control register; does nothing without one
        static void Write(Word value);
        /// Whether the governed bits are the policy's right now
        [[nodiscard]] static bool IsDeterministic();
        /// `current` with the governed bits replaced by the policy's and every other bit kept
        [[nodiscard]] static constexpr Word WithPolicy(const Word current)
        {
            return (current & ~kGovernedMask) | kPolicy;
        }
    };

    /**
     * Sets the policy for the life of the object and puts back what was there: construct it at the start of a fixed
     * tick in a network or deterministic simulation mode (Application::RunFixedTick does), so the tick runs under the
     * policy whatever a library changed in between. On every way out of the scope, an exception included, the governed
     * bits go back to the values they had at construction; the status flags (which exceptions happened) are left
     * as the tick made them. In the None simulation mode nothing constructs it, so nothing changes there.
     *
     * Verified() is false when, after writing the policy, the register does not read back as the policy (a target that
     * ignores the write): the caller reports it rather than simulating under unknown settings.
     */
    class FloatEnvironmentGuard
    {
    public:
        FloatEnvironmentGuard();
        ~FloatEnvironmentGuard();
        FloatEnvironmentGuard(const FloatEnvironmentGuard &) = delete;
        FloatEnvironmentGuard &operator=(const FloatEnvironmentGuard &) = delete;

        /// The register as it was before the guard set the policy
        [[nodiscard]] FloatEnvironment::Word Previous() const { return _previous; }
        /// Whether the policy is in force (read back after writing it)
        [[nodiscard]] bool Verified() const { return _verified; }

    private:
        FloatEnvironment::Word _previous;
        bool _verified;
    };
}
