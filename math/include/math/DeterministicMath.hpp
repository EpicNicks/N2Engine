#pragma once

namespace N2Engine::Math
{
    /**
     * Engine-owned transcendental functions (netcode C1, blocker B8 of the determinism report). The C library's sin,
     * cos, atan2, exp and log differ in the last bit between the MSVC CRT, glibc, musl (Emscripten) and Apple's libm, so
     * a simulation that calls them gives different results on different platforms. These do not call the C library's
     * transcendental functions at all: each is a fixed sequence of IEEE-754 double additions, subtractions,
     * multiplications, divisions and square roots (every one correctly rounded, so identical on every conforming
     * target), in a fixed order, compiled with floating-point contraction off (math/CMakeLists.txt sets
     * -ffp-contract=off, and /fp:precise for MSVC, on the one source file that holds them, in every build whether or not
     * N2ENGINE_STRICT_FP is on). The same code answers every platform, so the results are bit for bit the same on MSVC,
     * GCC, Clang, arm64 and wasm (tests/math/math_types/DeterministicMathTests.cpp pins exact bit patterns).
     *
     * Two precisions. The double functions are the definition (Lua numbers are doubles). The float functions compute
     * the double result and round it once to float, so they are the double function's value, not a separate algorithm.
     *
     * Accuracy, measured against a 90-digit reference over about 72000 sampled arguments (the sweeps are in
     * docs/math.html) and asserted as bounds by the tests:
     *   float:  within 1 ulp of the exact result (every sampled result was the correctly rounded float, 0.5 ulp).
     *   double: Sin, Cos 2 ulp (measured 1.8), Tan 3 (2.8), Exp, Log, Log2 2 (1.3), Atan, Atan2, Asin, Acos 4 (3.9).
     *   This includes arguments within 1e-18 of a multiple of pi/2: the reduction below keeps a relative error of a
     *   few ulp there too (pi/2 is carried to about 1e-47).
     * They are documented bounds, not exact-rounding guarantees.
     *
     * Argument reduction (Sin, Cos, Tan): |x| up to 2^20 (1048576) is reduced by a three-part Cody-Waite subtraction of
     * the nearest multiple of pi/2, accurate as stated above. Beyond that the argument is first reduced by
     * fmod(x, 2*pi) with the double nearest 2*pi (fmod is exact on every platform), so the result is the same
     * everywhere but is only accurate to about |x| * 4e-17 in absolute terms (the error of that double as 2*pi, per radian); it
     * is not the sine of the exact argument. Game angles never get there.
     *
     * Special values, the same everywhere: NaN in gives the one canonical quiet NaN out (bit pattern 0x7FF8000000000000
     * for double, 0x7FC00000 for float: the sign and payload of a NaN differ between x86 and arm, so no NaN passes
     * through); Sin, Cos, Tan, Asin, Acos of an infinity or Asin, Acos outside [-1, 1] are that NaN; Sin(+-0) = +-0,
     * Tan(+-0) = +-0, Cos(0) = 1; Atan(+-inf) = +-pi/2; Atan2 follows the IEEE/C table (Atan2(+-0, +x) = +-0,
     * Atan2(+-0, -x) = +-pi, Atan2(+-y, +-0) = +-pi/2, the infinities give +-pi/4, +-3pi/4, +-0, +-pi, +-pi/2);
     * Exp(+inf) = +inf, Exp(-inf) = 0, Exp overflows to +inf above about 709.78 and underflows through the denormals
     * to 0 (no flush: the floating-point environment guard keeps flush-to-zero off); Log(0) = -inf, Log(x < 0) = the
     * NaN, Log(+inf) = +inf, Log of a denormal is accurate; Log2(2^k) = k exactly.
     */

    // Double precision (the definition)
    [[nodiscard]] double Sin(double x);
    [[nodiscard]] double Cos(double x);
    [[nodiscard]] double Tan(double x);
    [[nodiscard]] double Asin(double x);
    [[nodiscard]] double Acos(double x);
    [[nodiscard]] double Atan(double x);
    /// The angle of the point (x, y) in (-pi, pi], as the C library's atan2(y, x): note y first
    [[nodiscard]] double Atan2(double y, double x);
    [[nodiscard]] double Exp(double x);
    /// Natural logarithm
    [[nodiscard]] double Log(double x);
    [[nodiscard]] double Log2(double x);

    // Single precision: the double result rounded once to float
    [[nodiscard]] float Sin(float x);
    [[nodiscard]] float Cos(float x);
    [[nodiscard]] float Tan(float x);
    [[nodiscard]] float Asin(float x);
    [[nodiscard]] float Acos(float x);
    [[nodiscard]] float Atan(float x);
    [[nodiscard]] float Atan2(float y, float x);
    [[nodiscard]] float Exp(float x);
    [[nodiscard]] float Log(float x);
    [[nodiscard]] float Log2(float x);

    /**
     * Whether the engine-owned functions of this build run without floating-point contraction: a multiply-add that the
     * compiler fused into one rounding (FMA contraction) would change their last bits on some targets, so the answer
     * is measured, not assumed, by running a multiply-add whose fused and separate results differ, inside the one
     * source file that holds the functions (so with exactly their compile flags). True on every supported build; it is
     * what the Deterministic network preset's "engine-owned trig" requirement checks.
     */
    [[nodiscard]] bool IsContractionFree();
}
