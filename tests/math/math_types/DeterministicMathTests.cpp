#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include <math/Constants.hpp>
#include <math/DeterministicMath.hpp>
#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Random.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

// The engine-owned transcendental functions (netcode C1): Math::Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Exp, Log, Log2.
//
// Three kinds of check:
//  - Exact bit patterns. The tables below were computed once from the implementation and cross-checked against the C
//    library and a 90-digit reference (every entry but the two arguments beyond 2^20 is the correctly rounded float, or
//    within 2 ulp for double). They are the same on every leg of the CI matrix (MSVC, GCC, Clang, arm64, wasm): that
//    identical-everywhere is the point, so a result that differs in one bit on one platform fails here.
//  - Accuracy: the documented bounds against a double-double (about 106 bit) reference computed in this file, on sweeps.
//  - Special values and structure: NaN, infinities, zeros, domain errors, symmetry, monotonicity, periodicity.
//
// To regenerate a table after a deliberate change to an algorithm, print the bits of the new results and replace the table
// (the old and new values go in the pull request).

using namespace N2Engine;

namespace
{
    std::uint32_t Bits(const float value)
    {
        return std::bit_cast<std::uint32_t>(value);
    }

    std::uint64_t Bits(const double value)
    {
        return std::bit_cast<std::uint64_t>(value);
    }

    float FloatOf(const std::uint32_t bits)
    {
        return std::bit_cast<float>(bits);
    }

    double DoubleOf(const std::uint64_t bits)
    {
        return std::bit_cast<double>(bits);
    }

    constexpr std::uint64_t kCanonicalNaN = 0x7FF8000000000000ull;
    constexpr std::uint32_t kCanonicalNaNf = 0x7FC00000u;

    // The distance between two numbers in units in the last place of the second one (the reference)
    double UlpError(const double value, const double reference)
    {
        const double magnitude = std::fabs(reference);
        const double ulp = std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
        return std::fabs(value - reference) / ulp;
    }

    double UlpErrorf(const float value, const float reference)
    {
        const float magnitude = std::fabs(reference);
        const float ulp = std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude;
        return static_cast<double>(std::fabs(value - reference)) / static_cast<double>(ulp);
    }

    // A deterministic source of arguments: xorshift64*, mapped to [0, 1) with 53 bits
    class Sweep
    {
    public:
        explicit Sweep(const std::uint64_t seed) : _state(seed) {}

        double Unit()
        {
            _state ^= _state >> 12;
            _state ^= _state << 25;
            _state ^= _state >> 27;
            const std::uint64_t r = _state * 0x2545F4914F6CDD1Dull;
            return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
        }

        double Uniform(const double low, const double high) { return low + (high - low) * Unit(); }

    private:
        std::uint64_t _state;
    };

    // -- A high-precision reference: double-double arithmetic (about 106 bits). The C library's value is only the starting
    // guess of a Newton step (or the log mantissa), which the step then corrects to double-double accuracy, so the
    // platform's own last-bit error does not reach the reference. Only additions, std::fma and divisions are used
    // otherwise, so the reference is the same on every platform.
    struct DD
    {
        double hi;
        double lo;
    };

    DD TwoSum(const double a, const double b)
    {
        const double s = a + b;
        const double bb = s - a;
        return {s, (a - (s - bb)) + (b - bb)};
    }

    DD QuickTwoSum(const double a, const double b)
    {
        const double s = a + b;
        return {s, b - (s - a)};
    }

    DD DDAdd(const DD a, const DD b)
    {
        DD s = TwoSum(a.hi, b.hi);
        const DD t = TwoSum(a.lo, b.lo);
        s = QuickTwoSum(s.hi, s.lo + t.hi);
        return QuickTwoSum(s.hi, s.lo + t.lo);
    }

    DD DDNeg(const DD a) { return {-a.hi, -a.lo}; }

    DD DDSub(const DD a, const DD b) { return DDAdd(a, DDNeg(b)); }

    DD DDMul(const DD a, const DD b)
    {
        const double p = a.hi * b.hi;
        const double e = std::fma(a.hi, b.hi, -p);
        return QuickTwoSum(p, e + (a.hi * b.lo + a.lo * b.hi));
    }

    DD DDMulD(const DD a, const double b) { return DDMul(a, DD{b, 0.0}); }

    DD DDDiv(const DD a, const DD b)
    {
        const double q1 = a.hi / b.hi;
        DD r = DDSub(a, DDMulD(b, q1));
        const double q2 = r.hi / b.hi;
        r = DDSub(r, DDMulD(b, q2));
        const double q3 = r.hi / b.hi;
        const DD q = QuickTwoSum(q1, q2);
        return DDAdd(q, DD{q3, 0.0});
    }

    // pi/2 and ln 2 to about 1e-49 (three doubles each)
    constexpr double kHalfPiHi = 1.5707963267948965580e+00;
    constexpr double kHalfPiMid = 6.1232339957367660359e-17;
    constexpr double kHalfPiLo = -1.4973849048591698e-33;
    constexpr double kLn2Hi = 6.9314718055994528623e-01;
    constexpr double kLn2Mid = 2.3190468138462995584e-17;
    constexpr double kLn2Lo = 5.7077084384162120658e-34;

    // sin r and cos r for |r| <= 0.8, Taylor series summed in double-double
    void SinCosSmall(const DD r, DD &sine, DD &cosine)
    {
        const DD r2 = DDMul(r, r);
        DD term = r;
        sine = r;
        for (int n = 1; n < 40; ++n)
        {
            term = DDNeg(DDDiv(DDMul(term, r2), DD{static_cast<double>((2 * n) * (2 * n + 1)), 0.0}));
            sine = DDAdd(sine, term);
        }
        term = DD{1.0, 0.0};
        cosine = term;
        for (int n = 1; n < 40; ++n)
        {
            term = DDNeg(DDDiv(DDMul(term, r2), DD{static_cast<double>((2 * n - 1) * (2 * n)), 0.0}));
            cosine = DDAdd(cosine, term);
        }
    }

    // a * b exactly, as the rounded product and its error
    DD TwoProduct(const double a, const double b)
    {
        const double p = a * b;
        return {p, std::fma(a, b, -p)};
    }

    // sin x and cos x of a double, |x| up to a few million. k times each part of pi/2 is taken exactly (TwoProduct): a
    // rounded k * kHalfPiMid would leave an error of about k * 1e-32, which is hundreds of ulp of the reduced argument
    // next to a multiple of pi/2 (where it is about 1e-18)
    void SinCosDD(const double x, DD &sine, DD &cosine)
    {
        const double k = std::nearbyint(x / kHalfPiHi);
        DD r = DDSub(DD{x, 0.0}, TwoProduct(k, kHalfPiHi));
        r = DDSub(r, TwoProduct(k, kHalfPiMid));
        r = DDSub(r, DD{k * kHalfPiLo, 0.0});
        DD s;
        DD c;
        SinCosSmall(r, s, c);
        long long quadrant = static_cast<long long>(k) % 4;
        if (quadrant < 0)
        {
            quadrant += 4;
        }
        switch (quadrant)
        {
        case 0: sine = s; cosine = c; break;
        case 1: sine = c; cosine = DDNeg(s); break;
        case 2: sine = DDNeg(s); cosine = DDNeg(c); break;
        default: sine = DDNeg(c); cosine = s; break;
        }
    }

    DD RefSin(const double x) { DD s; DD c; SinCosDD(x, s, c); return s; }
    DD RefCos(const double x) { DD s; DD c; SinCosDD(x, s, c); return c; }
    DD RefTan(const double x) { DD s; DD c; SinCosDD(x, s, c); return DDDiv(s, c); }

    // exp x for |x| below 745: x = k ln2 + r, Taylor series of r, scaled by 2^k
    DD RefExp(const double x)
    {
        const double k = std::nearbyint(x / kLn2Hi);
        DD r = DDSub(DD{x, 0.0}, DDMulD(DD{kLn2Hi, kLn2Mid}, k));
        r = DDSub(r, DD{k * kLn2Lo, 0.0});
        DD term{1.0, 0.0};
        DD sum{1.0, 0.0};
        for (int n = 1; n < 40; ++n)
        {
            term = DDDiv(DDMul(term, r), DD{static_cast<double>(n), 0.0});
            sum = DDAdd(sum, term);
        }
        const int e = static_cast<int>(k);
        return {std::ldexp(sum.hi, e), std::ldexp(sum.lo, e)};
    }

    // ln x for a positive finite x: x = m 2^e, one Newton step on exp from the C library's guess for ln m
    DD RefLog(const double x)
    {
        int e = 0;
        const double m = std::frexp(x, &e); // [0.5, 1)
        const double guess = std::log(m);
        const DD t = DDSub(DDMul(DD{m, 0.0}, RefExp(-guess)), DD{1.0, 0.0});
        DD y = DDAdd(DD{guess, 0.0}, t);
        y = DDSub(y, DDMulD(DDMul(t, t), 0.5));
        return DDAdd(y, DDMulD(DD{kLn2Hi, kLn2Mid}, static_cast<double>(e)));
    }

    DD RefLog2(const double x) { return DDDiv(RefLog(x), DD{kLn2Hi, kLn2Mid}); }

    // One Newton step t - f(t) / f'(t) in double-double from a double guess t
    DD Newton(const double t, const DD f, const DD fprime)
    {
        return DDSub(DD{t, 0.0}, DDDiv(f, fprime));
    }

    DD RefAtan(const double x)
    {
        const double t = std::atan(x);
        DD s;
        DD c;
        SinCosDD(t, s, c);
        return Newton(t, DDSub(s, DDMulD(c, x)), DDAdd(c, DDMulD(s, x)));
    }

    DD RefAtan2(const double y, const double x)
    {
        const double t = std::atan2(y, x);
        DD s;
        DD c;
        SinCosDD(t, s, c);
        return Newton(t, DDSub(DDMulD(s, x), DDMulD(c, y)), DDAdd(DDMulD(c, x), DDMulD(s, y)));
    }

    DD RefAsin(const double x)
    {
        const double t = std::asin(x);
        DD s;
        DD c;
        SinCosDD(t, s, c);
        return Newton(t, DDSub(s, DD{x, 0.0}), c);
    }

    DD RefAcos(const double x)
    {
        const double t = std::acos(x);
        DD s;
        DD c;
        SinCosDD(t, s, c);
        return Newton(t, DDSub(c, DD{x, 0.0}), DDNeg(s));
    }

    // The distance between a result and a double-double reference, in units in the last place of the reference
    double UlpErrorDD(const double value, const DD reference)
    {
        const double magnitude = std::fabs(reference.hi);
        const double ulp = std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
        return std::fabs((value - reference.hi) - reference.lo) / ulp;
    }

    // The same for a float result: the reference is rounded once to float, the distance is in float ulps
    double UlpErrorfDD(const float value, const DD reference)
    {
        return UlpErrorf(value, static_cast<float>(reference.hi));
    }

    struct FloatCase { std::uint32_t x; std::uint32_t expected; };
    struct Float2Case { std::uint32_t y; std::uint32_t x; std::uint32_t expected; };
    struct DoubleCase { std::uint64_t x; std::uint64_t expected; };
    struct Double2Case { std::uint64_t y; std::uint64_t x; std::uint64_t expected; };

    constexpr FloatCase kFloatSin[] = {
        {0x3DCCCCCDu, 0x3DCC7577u},
        {0x3F000000u, 0x3EF57744u},
        {0x3F800000u, 0x3F576AA4u},
        {0x3FC90FDBu, 0x3F800000u},
        {0x40000000u, 0x3F68C7B7u},
        {0x40490FDBu, 0xB3BBBD2Eu},
        {0x40900000u, 0xBF7A3F6Au},
        {0x41200000u, 0xBF0B44F8u},
        {0x42C80000u, 0xBF01A12Eu},
        {0x447A2000u, 0x3F7ECA46u},
        {0x4640E6B6u, 0xBF344B08u},
        {0xBF333333u, 0xBF24EB73u},
        {0xC0400000u, 0xBE1081C3u},
        {0xC2C88000u, 0x3E8DF805u},
        {0x3727C5ACu, 0x3727C5ACu},
        {0x322BCC77u, 0x322BCC77u},
        {0x47800000u, 0x3F312B34u},
        {0x49800000u, 0x3EA93666u},
        {0x49F42400u, 0xBF27DCE5u},
        {0x4E6E6B28u, 0x3F0BBC66u},
        {0x3F490FDBu, 0x3F3504F3u},
        {0x40C90FDBu, 0x343BBD2Eu},
    };

    constexpr FloatCase kFloatCos[] = {
        {0x3DCCCCCDu, 0x3F7EB898u},
        {0x3F000000u, 0x3F60A940u},
        {0x3F800000u, 0x3F0A5140u},
        {0x3FC90FDBu, 0xB33BBD2Eu},
        {0x40000000u, 0xBED51133u},
        {0x40490FDBu, 0xBF800000u},
        {0x40900000u, 0xBE57DADBu},
        {0x41200000u, 0xBF56CD64u},
        {0x42C80000u, 0x3F5CC0EEu},
        {0x447A2000u, 0x3DC6DFFCu},
        {0x4640E6B6u, 0x3F35BE20u},
        {0xBF333333u, 0x3F43CCB3u},
        {0xC0400000u, 0xBF7D7026u},
        {0xC2C88000u, 0x3F75F639u},
        {0x3727C5ACu, 0x3F800000u},
        {0x322BCC77u, 0x3F800000u},
        {0x47800000u, 0xBF38CA2Au},
        {0x49800000u, 0x3F719D6Du},
        {0x49F42400u, 0x3F414847u},
        {0x4E6E6B28u, 0x3F567FC6u},
        {0x3F490FDBu, 0x3F3504F3u},
        {0x40C90FDBu, 0x3F800000u},
    };

    constexpr FloatCase kFloatTan[] = {
        {0x3DCCCCCDu, 0x3DCD7C44u},
        {0x3F000000u, 0x3F0BDA7Bu},
        {0x3F800000u, 0x3FC75923u},
        {0x3FC90FDBu, 0xCBAE8A4Au},
        {0x40000000u, 0xC00BD7B1u},
        {0x40490FDBu, 0x33BBBD2Eu},
        {0x40900000u, 0x40946506u},
        {0x41200000u, 0x3F25FAFAu},
        {0x42C80000u, 0xBF1653A7u},
        {0x447A2000u, 0x4123FCF9u},
        {0x4640E6B6u, 0xBF7DF549u},
        {0xBF333333u, 0xBF57A036u},
        {0xC0400000u, 0x3E11F7B9u},
        {0xC2C88000u, 0x3E93C34Au},
        {0x3727C5ACu, 0x3727C5ACu},
        {0x322BCC77u, 0x322BCC77u},
        {0x47800000u, 0xBF757138u},
        {0x49800000u, 0x3EB34973u},
        {0x49F42400u, 0xBF5E550Eu},
        {0x4E6E6B28u, 0x3F26C593u},
        {0x3F490FDBu, 0x3F800000u},
        {0x40C90FDBu, 0x343BBD2Eu},
    };

    constexpr FloatCase kFloatAsin[] = {
        {0xBF800000u, 0xBFC90FDBu},
        {0xBF666666u, 0xBF8F549Bu},
        {0xBF000000u, 0xBF060A92u},
        {0xBDCCCCCDu, 0xBDCD2494u},
        {0x00000000u, 0x00000000u},
        {0x3DCCCCCDu, 0x3DCD2494u},
        {0x3E99999Au, 0x3E9C00ADu},
        {0x3F000000u, 0x3F060A92u},
        {0x3F3504F3u, 0x3F490FDAu},
        {0x3F666666u, 0x3F8F549Bu},
        {0x3F7FFF58u, 0x3FC87D36u},
        {0x3F800000u, 0x3FC90FDBu},
    };

    constexpr FloatCase kFloatAcos[] = {
        {0xBF800000u, 0x40490FDBu},
        {0xBF666666u, 0x402C323Bu},
        {0xBF000000u, 0x40060A92u},
        {0xBDCCCCCDu, 0x3FD5E224u},
        {0x00000000u, 0x3FC90FDBu},
        {0x3DCCCCCDu, 0x3FBC3D91u},
        {0x3E99999Au, 0x3FA20FAFu},
        {0x3F000000u, 0x3F860A92u},
        {0x3F3504F3u, 0x3F490FDBu},
        {0x3F666666u, 0x3EE6ECFEu},
        {0x3F7FFF58u, 0x3B92A47Eu},
        {0x3F800000u, 0x00000000u},
    };

    constexpr FloatCase kFloatAtan[] = {
        {0xC47A0000u, 0xBFC8EF16u},
        {0xC0400000u, 0xBF9FE0BBu},
        {0xBF800000u, 0xBF490FDBu},
        {0xBF000000u, 0xBEED6338u},
        {0x3DCCCCCDu, 0x3DCC1F14u},
        {0x3F000000u, 0x3EED6338u},
        {0x3F800000u, 0x3F490FDBu},
        {0x40000000u, 0x3F8DB70Du},
        {0x41200000u, 0x3FBC4DE9u},
        {0x49742400u, 0x3FC90FD2u},
        {0x3727C5ACu, 0x3727C5ACu},
    };

    constexpr FloatCase kFloatExp[] = {
        {0xC2A00000u, 0x05BFECBAu},
        {0xC1A00000u, 0x310DA433u},
        {0xC0A00000u, 0x3BDCC9FFu},
        {0xBF800000u, 0x3EBC5AB2u},
        {0xBDCCCCCDu, 0x3F67A36Du},
        {0x00000000u, 0x3F800000u},
        {0x3DCCCCCDu, 0x3F8D763Eu},
        {0x3F000000u, 0x3FD3094Cu},
        {0x3F800000u, 0x402DF854u},
        {0x40000000u, 0x40EC7326u},
        {0x41200000u, 0x46AC14EEu},
        {0x42480000u, 0x638C881Fu},
        {0x42B00000u, 0x7EF882B7u},
    };

    constexpr FloatCase kFloatLog[] = {
        {0x0DA24260u, 0xC28A27B5u},
        {0x3C23D70Au, 0xC0935D8Eu},
        {0x3DCCCCCDu, 0xC0135D8Eu},
        {0x3F000000u, 0xBF317218u},
        {0x3F800000u, 0x00000000u},
        {0x3FC00000u, 0x3ECF991Fu},
        {0x40000000u, 0x3F317218u},
        {0x41000000u, 0x40051592u},
        {0x41200000u, 0x40135D8Eu},
        {0x4640E400u, 0x4116BC71u},
        {0x60AD78ECu, 0x423834F1u},
    };

    constexpr FloatCase kFloatLog2[] = {
        {0x0DA24260u, 0xC2C750D1u},
        {0x3C23D70Au, 0xC0D49A78u},
        {0x3DCCCCCDu, 0xC0549A78u},
        {0x3F000000u, 0xBF800000u},
        {0x3F800000u, 0x00000000u},
        {0x3FC00000u, 0x3F15C01Au},
        {0x40000000u, 0x3F800000u},
        {0x41000000u, 0x40400000u},
        {0x41200000u, 0x40549A78u},
        {0x4640E400u, 0x4159775Bu},
        {0x60AD78ECu, 0x4284E08Bu},
    };

    constexpr Float2Case kFloatAtan2[] = {
        {0x3F800000u, 0x3F800000u, 0x3F490FDBu},
        {0x3F800000u, 0xBF800000u, 0x4016CBE4u},
        {0xBF800000u, 0xBF800000u, 0xC016CBE4u},
        {0xBF800000u, 0x3F800000u, 0xBF490FDBu},
        {0x3F000000u, 0x40000000u, 0x3E7ADBB0u},
        {0x40400000u, 0x3DCCCCCDu, 0x3FC4CBFEu},
        {0xC0E00000u, 0x40800000u, 0xBF869C79u},
        {0x3A83126Fu, 0x3F800000u, 0x3A83126Cu},
        {0x40000000u, 0x00000000u, 0x3FC90FDBu},
        {0x00000000u, 0xBF800000u, 0x40490FDBu},
    };

    constexpr DoubleCase kDoubleSin[] = {
        {0x3FB999999999999Aull, 0x3FB98EAECB8BCB2Cull},
        {0x3FE0000000000000ull, 0x3FDEAEE8744B05F0ull},
        {0x3FF0000000000000ull, 0x3FEAED548F090CEEull},
        {0x4000000000000000ull, 0x3FED18F6EAD1B446ull},
        {0x400921FB54442D18ull, 0x3CA1A62633145C07ull},
        {0x4012000000000000ull, 0xBFEF47ED3DC74080ull},
        {0x4024000000000000ull, 0xBFE1689EF5F34F52ull},
        {0x4059000000000000ull, 0xBFE03425B78C4DB8ull},
        {0x408F440000000000ull, 0x3FEFD948C50A7A0Dull},
        {0x40C81CD6C8B43958ull, 0xBFE687D5890974A5ull},
        {0xBFE6666666666666ull, 0xBFE49D6E694619B8ull},
        {0xC008000000000000ull, 0xBFC210386DB6D55Bull},
        {0x3EE4F8B588E368F1ull, 0x3EE4F8B588E1E8A2ull},
        {0x3E45798EE2308C3Aull, 0x3E45798EE2308C3Aull},
        {0x40F0000000000000ull, 0x3FE62566735B8513ull},
        {0x4130000000000000ull, 0x3FD526CCB2FC8656ull},
        {0x413E848000000000ull, 0xBFE4FB9C96938FE0ull},
        {0x41CDCD6500000000ull, 0x3FE1778CC00CD87Aull},
        {0x3FE921FB54442D18ull, 0x3FE6A09E667F3BCCull},
        {0x401921FB54442D18ull, 0xBCB1A62633145C07ull},
        {0x3FF921FB54442D18ull, 0x3FF0000000000000ull},
    };

    constexpr DoubleCase kDoubleCos[] = {
        {0x3FB999999999999Aull, 0x3FEFD712F9A817C1ull},
        {0x3FE0000000000000ull, 0x3FEC1528065B7D50ull},
        {0x3FF0000000000000ull, 0x3FE14A280FB5068Cull},
        {0x4000000000000000ull, 0xBFDAA22657537205ull},
        {0x400921FB54442D18ull, 0xBFF0000000000000ull},
        {0x4012000000000000ull, 0xBFCAFB5B54583D6Aull},
        {0x4024000000000000ull, 0xBFEAD9AC890C6B1Full},
        {0x4059000000000000ull, 0x3FEB981DBF665FDFull},
        {0x408F440000000000ull, 0x3FB8DBFF75EB664Full},
        {0x40C81CD6C8B43958ull, 0x3FE6B94C3BBE24B8ull},
        {0xBFE6666666666666ull, 0x3FE87996529F9D93ull},
        {0xC008000000000000ull, 0xBFEFAE04BE85E5D2ull},
        {0x3EE4F8B588E368F1ull, 0x3FEFFFFFFFF920C8ull},
        {0x3E45798EE2308C3Aull, 0x3FF0000000000000ull},
        {0x40F0000000000000ull, 0xBFE719453109288Bull},
        {0x4130000000000000ull, 0x3FEE33ADA92FE2AEull},
        {0x413E848000000000ull, 0x3FE82908D66F77F2ull},
        {0x41CDCD6500000000ull, 0x3FEACFF8BBC9D814ull},
        {0x3FE921FB54442D18ull, 0x3FE6A09E667F3BCDull},
        {0x401921FB54442D18ull, 0x3FF0000000000000ull},
        {0x3FF921FB54442D18ull, 0x3C91A62633145C07ull},
    };

    constexpr DoubleCase kDoubleTan[] = {
        {0x3FB999999999999Aull, 0x3FB9AF8877430B7Full},
        {0x3FE0000000000000ull, 0x3FE17B4F5BF3474Aull},
        {0x3FF0000000000000ull, 0x3FF8EB245CBEE3A5ull},
        {0x4000000000000000ull, 0xC0017AF62E0950F8ull},
        {0x400921FB54442D18ull, 0xBCA1A62633145C07ull},
        {0x4012000000000000ull, 0x40128CA0C62BF595ull},
        {0x4024000000000000ull, 0x3FE4BF5F34BE3782ull},
        {0x4059000000000000ull, 0xBFE2CA74D62B5D38ull},
        {0x408F440000000000ull, 0x40247F9F1C1F4871ull},
        {0x40C81CD6C8B43958ull, 0xBFEFBA5836323A4Dull},
        {0xBFE6666666666666ull, 0xBFEAF406C2FC78ADull},
        {0xC008000000000000ull, 0x3FC23EF71254B86Full},
        {0x3EE4F8B588E368F1ull, 0x3EE4F8B588E6698Eull},
        {0x3E45798EE2308C3Aull, 0x3E45798EE2308C3Aull},
        {0x40F0000000000000ull, 0xBFEEAE2708CF5425ull},
        {0x4130000000000000ull, 0x3FD6692E57792070ull},
        {0x413E848000000000ull, 0xBFEBCAA1B8A38A29ull},
        {0x41CDCD6500000000ull, 0x3FE4D8B267B32DE5ull},
        {0x3FE921FB54442D18ull, 0x3FEFFFFFFFFFFFFFull},
        {0x401921FB54442D18ull, 0xBCB1A62633145C07ull},
        {0x3FF921FB54442D18ull, 0x434D02967C31CDB5ull},
    };

    constexpr DoubleCase kDoubleAsin[] = {
        {0xBFF0000000000000ull, 0xBFF921FB54442D18ull},
        {0xBFECCCCCCCCCCCCDull, 0xBFF1EA93705FA172ull},
        {0xBFE0000000000000ull, 0xBFE0C152382D7366ull},
        {0xBFB999999999999Aull, 0xBFB9A49276037884ull},
        {0x0000000000000000ull, 0x0000000000000000ull},
        {0x3FB999999999999Aull, 0x3FB9A49276037884ull},
        {0x3FD3333333333333ull, 0x3FD380159E14F6FFull},
        {0x3FE0000000000000ull, 0x3FE0C152382D7366ull},
        {0x3FE6A09E667F3BCDull, 0x3FE921FB54442D19ull},
        {0x3FECCCCCCCCCCCCDull, 0x3FF1EA93705FA172ull},
        {0x3FEFFFEB074A771Dull, 0x3FF90FA9F3695AA5ull},
        {0x3FF0000000000000ull, 0x3FF921FB54442D18ull},
    };

    constexpr DoubleCase kDoubleAcos[] = {
        {0xBFF0000000000000ull, 0x400921FB54442D18ull},
        {0xBFECCCCCCCCCCCCDull, 0x400586476251E745ull},
        {0xBFE0000000000000ull, 0x4000C152382D7366ull},
        {0xBFB999999999999Aull, 0x3FFABC447BA464A1ull},
        {0x0000000000000000ull, 0x3FF921FB54442D18ull},
        {0x3FB999999999999Aull, 0x3FF787B22CE3F590ull},
        {0x3FD3333333333333ull, 0x3FF441F5ECBEEF58ull},
        {0x3FE0000000000000ull, 0x3FF0C152382D7365ull},
        {0x3FE6A09E667F3BCDull, 0x3FE921FB54442D18ull},
        {0x3FECCCCCCCCCCCCDull, 0x3FDCDD9F8F922E98ull},
        {0x3FEFFFEB074A771Dull, 0x3F725160DAD27315ull},
        {0x3FF0000000000000ull, 0x0000000000000000ull},
    };

    constexpr DoubleCase kDoubleAtan[] = {
        {0xC08F400000000000ull, 0xBFF91DE2C0E658BDull},
        {0xC008000000000000ull, 0xBFF3FC176B7A8560ull},
        {0xBFF0000000000000ull, 0xBFE921FB54442D18ull},
        {0xBFE0000000000000ull, 0xBFDDAC670561BB4Full},
        {0x3FB999999999999Aull, 0x3FB983E282E2CC4Dull},
        {0x3FE0000000000000ull, 0x3FDDAC670561BB4Full},
        {0x3FF0000000000000ull, 0x3FE921FB54442D18ull},
        {0x4000000000000000ull, 0x3FF1B6E192EBBE44ull},
        {0x4024000000000000ull, 0x3FF789BD2C160053ull},
        {0x412E848000000000ull, 0x3FF921FA47D4B30Dull},
        {0x3EE4F8B588E368F1ull, 0x3EE4F8B588E06854ull},
    };

    constexpr DoubleCase kDoubleExp[] = {
        {0xC085E00000000000ull, 0x00D14F2B0FB9307Full},
        {0xC054000000000000ull, 0x38B7FD974D372E44ull},
        {0xC034000000000000ull, 0x3E21B48655F37266ull},
        {0xC014000000000000ull, 0x3F7B993FE00D5376ull},
        {0xBFF0000000000000ull, 0x3FD78B56362CEF38ull},
        {0xBFB999999999999Aull, 0x3FECF46D99D52B3Bull},
        {0x0000000000000000ull, 0x3FF0000000000000ull},
        {0x3FB999999999999Aull, 0x3FF1AEC7B35A00D4ull},
        {0x3FE0000000000000ull, 0x3FFA61298E1E069Cull},
        {0x3FF0000000000000ull, 0x4005BF0A8B145769ull},
        {0x4000000000000000ull, 0x401D8E64B8D4DDADull},
        {0x4024000000000000ull, 0x40D5829DCF950560ull},
        {0x4049000000000000ull, 0x44719103E4080B45ull},
        {0x4056000000000000ull, 0x47DF1056DC7BF22Dull},
        {0x4085E00000000000ull, 0x7F0D945DF4F8EC8Eull},
        {0x4086280000000000ull, 0x7FDD422D2BE5DC9Aull},
    };

    constexpr DoubleCase kDoubleLog[] = {
        {0x01A56E1FC2F8F359ull, 0xC085963447F87FB5ull},
        {0x39B4484BFEEBC2A0ull, 0xC05144F69FF9FFC4ull},
        {0x3F847AE147AE147Bull, 0xC0126BB1BBB55515ull},
        {0x3FB999999999999Aull, 0xC0026BB1BBB55515ull},
        {0x3FE0000000000000ull, 0xBFE62E42FEFA39EFull},
        {0x3FF0000000000000ull, 0x0000000000000000ull},
        {0x3FF8000000000000ull, 0x3FD9F323ECBF984Cull},
        {0x4000000000000000ull, 0x3FE62E42FEFA39EFull},
        {0x4020000000000000ull, 0x4000A2B23F3BAB73ull},
        {0x4024000000000000ull, 0x40026BB1BBB55515ull},
        {0x40C81C8000000000ull, 0x4022D78E26AE18E7ull},
        {0x4415AF1D78B58C40ull, 0x4047069E2AA2AA5Bull},
        {0x7E37E43C8800759Cull, 0x4085963447F87FB5ull},
    };

    constexpr DoubleCase kDoubleLog2[] = {
        {0x01A56E1FC2F8F359ull, 0xC08F24A09F1A8B89ull},
        {0x39B4484BFEEBC2A0ull, 0xC058EA1A18E2093Aull},
        {0x3F847AE147AE147Bull, 0xC01A934F0979A371ull},
        {0x3FB999999999999Aull, 0xC00A934F0979A371ull},
        {0x3FE0000000000000ull, 0xBFF0000000000000ull},
        {0x3FF0000000000000ull, 0x0000000000000000ull},
        {0x3FF8000000000000ull, 0x3FE2B803473F7AD2ull},
        {0x4000000000000000ull, 0x3FF0000000000000ull},
        {0x4020000000000000ull, 0x4008000000000000ull},
        {0x4024000000000000ull, 0x400A934F0979A371ull},
        {0x40C81C8000000000ull, 0x402B2EEB55D7C2D7ull},
        {0x4415AF1D78B58C40ull, 0x40509C1165EC0627ull},
        {0x7E37E43C8800759Cull, 0x408F24A09F1A8B89ull},
    };

    constexpr Double2Case kDoubleAtan2[] = {
        {0x3FF0000000000000ull, 0x3FF0000000000000ull, 0x3FE921FB54442D18ull},
        {0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x4002D97C7F3321D2ull},
        {0xBFF0000000000000ull, 0xBFF0000000000000ull, 0xC002D97C7F3321D2ull},
        {0xBFF0000000000000ull, 0x3FF0000000000000ull, 0xBFE921FB54442D18ull},
        {0x3FE0000000000000ull, 0x4000000000000000ull, 0x3FCF5B75F92C80DDull},
        {0x4008000000000000ull, 0x3FB999999999999Aull, 0x3FF8997FBB8B19C0ull},
        {0xC01C000000000000ull, 0x4010000000000000ull, 0xBFF0D38F2C5BA09Eull},
        {0x3F50624DD2F1A9FCull, 0x3FF0000000000000ull, 0x3F50624D77516E16ull},
        {0x4000000000000000ull, 0x0000000000000000ull, 0x3FF921FB54442D18ull},
        {0x0000000000000000ull, 0xBFF0000000000000ull, 0x400921FB54442D18ull},
        {0x01A56E1FC2F8F359ull, 0xFE37E43C8800759Cull, 0x400921FB54442D18ull},
        {0x4014000000000000ull, 0x4028000000000000ull, 0x3FD94441F8F7260Cull},
    };
}

// -- the build -------------------------------------------------------------------------------------------------------

TEST(DeterministicMathTest, TheFunctionsRunWithoutFloatingPointContraction)
{
    // Measured in the file that holds the functions, with exactly its compile flags: a fused multiply-add there would
    // change the last bits on the targets that have one
    EXPECT_TRUE(Math::IsContractionFree());
}

// -- exact bit patterns ----------------------------------------------------------------------------------------------

#define N2_EXPECT_FLOAT_TABLE(function, table)                                                                      \
    for (const FloatCase &entry : table)                                                                            \
    {                                                                                                               \
        const float x = FloatOf(entry.x);                                                                           \
        EXPECT_EQ(Bits(Math::function(x)), entry.expected)                                                          \
            << #function "(" << x << ") = " << Math::function(x) << ", expected " << FloatOf(entry.expected);       \
    }

#define N2_EXPECT_DOUBLE_TABLE(function, table)                                                                     \
    for (const DoubleCase &entry : table)                                                                           \
    {                                                                                                               \
        const double x = DoubleOf(entry.x);                                                                         \
        EXPECT_EQ(Bits(Math::function(x)), entry.expected)                                                          \
            << #function "(" << x << ") = " << Math::function(x) << ", expected " << DoubleOf(entry.expected);      \
    }

TEST(DeterministicMathTest, FloatResultsAreTheExactBitsOnEveryPlatform)
{
    N2_EXPECT_FLOAT_TABLE(Sin, kFloatSin)
    N2_EXPECT_FLOAT_TABLE(Cos, kFloatCos)
    N2_EXPECT_FLOAT_TABLE(Tan, kFloatTan)
    N2_EXPECT_FLOAT_TABLE(Asin, kFloatAsin)
    N2_EXPECT_FLOAT_TABLE(Acos, kFloatAcos)
    N2_EXPECT_FLOAT_TABLE(Atan, kFloatAtan)
    N2_EXPECT_FLOAT_TABLE(Exp, kFloatExp)
    N2_EXPECT_FLOAT_TABLE(Log, kFloatLog)
    N2_EXPECT_FLOAT_TABLE(Log2, kFloatLog2)
    for (const Float2Case &entry : kFloatAtan2)
    {
        const float y = FloatOf(entry.y);
        const float x = FloatOf(entry.x);
        EXPECT_EQ(Bits(Math::Atan2(y, x)), entry.expected) << "Atan2(" << y << ", " << x << ")";
    }
}

TEST(DeterministicMathTest, DoubleResultsAreTheExactBitsOnEveryPlatform)
{
    N2_EXPECT_DOUBLE_TABLE(Sin, kDoubleSin)
    N2_EXPECT_DOUBLE_TABLE(Cos, kDoubleCos)
    N2_EXPECT_DOUBLE_TABLE(Tan, kDoubleTan)
    N2_EXPECT_DOUBLE_TABLE(Asin, kDoubleAsin)
    N2_EXPECT_DOUBLE_TABLE(Acos, kDoubleAcos)
    N2_EXPECT_DOUBLE_TABLE(Atan, kDoubleAtan)
    N2_EXPECT_DOUBLE_TABLE(Exp, kDoubleExp)
    N2_EXPECT_DOUBLE_TABLE(Log, kDoubleLog)
    N2_EXPECT_DOUBLE_TABLE(Log2, kDoubleLog2)
    for (const Double2Case &entry : kDoubleAtan2)
    {
        const double y = DoubleOf(entry.y);
        const double x = DoubleOf(entry.x);
        EXPECT_EQ(Bits(Math::Atan2(y, x)), entry.expected) << "Atan2(" << y << ", " << x << ")";
    }
}

// The float functions are the double ones rounded once, nothing else
TEST(DeterministicMathTest, TheFloatFunctionsAreTheDoubleFunctionsRoundedOnce)
{
    Sweep sweep(7);
    for (int i = 0; i < 2000; ++i)
    {
        const float x = static_cast<float>(sweep.Uniform(-50.0, 50.0));
        const float unit = static_cast<float>(sweep.Uniform(-1.0, 1.0));
        EXPECT_EQ(Bits(Math::Sin(x)), Bits(static_cast<float>(Math::Sin(static_cast<double>(x)))));
        EXPECT_EQ(Bits(Math::Cos(x)), Bits(static_cast<float>(Math::Cos(static_cast<double>(x)))));
        EXPECT_EQ(Bits(Math::Tan(x)), Bits(static_cast<float>(Math::Tan(static_cast<double>(x)))));
        EXPECT_EQ(Bits(Math::Atan(x)), Bits(static_cast<float>(Math::Atan(static_cast<double>(x)))));
        EXPECT_EQ(Bits(Math::Asin(unit)), Bits(static_cast<float>(Math::Asin(static_cast<double>(unit)))));
        EXPECT_EQ(Bits(Math::Acos(unit)), Bits(static_cast<float>(Math::Acos(static_cast<double>(unit)))));
        EXPECT_EQ(Bits(Math::Atan2(x, unit)),
                  Bits(static_cast<float>(Math::Atan2(static_cast<double>(x), static_cast<double>(unit)))));
    }
}

// -- accuracy --------------------------------------------------------------------------------------------------------

// The reference is the double-double one above (about 106 bits; the C library's value only seeds a Newton step); the bound
// is the documented one of math/DeterministicMath.hpp: a float result is within 1 ulp of the exact result
TEST(DeterministicMathTest, FloatResultsAreWithinOneUlpOfTheReference)
{
    Sweep sweep(11);
    double worst = 0.0;
    auto check = [&worst](const char *name, const float x, const float actual, const DD reference) {
        const double error = UlpErrorfDD(actual, reference);
        worst = std::max(worst, error);
        EXPECT_LE(error, 1.0) << name << "(" << x << ") = " << actual << ", reference " << reference.hi;
    };
    for (int i = 0; i < 20000; ++i)
    {
        const float x = static_cast<float>(sweep.Uniform(-100.0, 100.0));
        check("Sin", x, Math::Sin(x), RefSin(static_cast<double>(x)));
        check("Cos", x, Math::Cos(x), RefCos(static_cast<double>(x)));
        check("Tan", x, Math::Tan(x), RefTan(static_cast<double>(x)));
        const float unit = static_cast<float>(sweep.Uniform(-1.0, 1.0));
        check("Asin", unit, Math::Asin(unit), RefAsin(static_cast<double>(unit)));
        check("Acos", unit, Math::Acos(unit), RefAcos(static_cast<double>(unit)));
        const float wide = static_cast<float>(sweep.Uniform(-1.0e6, 1.0e6));
        check("Atan", wide, Math::Atan(wide), RefAtan(static_cast<double>(wide)));
        check("Sin(wide)", wide, Math::Sin(wide), RefSin(static_cast<double>(wide)));
        const float e = static_cast<float>(sweep.Uniform(-80.0, 80.0));
        check("Exp", e, Math::Exp(e), RefExp(static_cast<double>(e)));
        const float positive = std::ldexp(static_cast<float>(sweep.Uniform(1.0, 2.0)), static_cast<int>(sweep.Uniform(-100.0, 100.0)));
        check("Log", positive, Math::Log(positive), RefLog(static_cast<double>(positive)));
        check("Log2", positive, Math::Log2(positive), RefLog2(static_cast<double>(positive)));
        const float y = static_cast<float>(sweep.Uniform(-10.0, 10.0));
        const float xx = static_cast<float>(sweep.Uniform(-10.0, 10.0));
        check("Atan2", y, Math::Atan2(y, xx), RefAtan2(static_cast<double>(y), static_cast<double>(xx)));
    }
    RecordProperty("worst_float_ulp", std::to_string(worst));
}

TEST(DeterministicMathTest, DoubleResultsAreWithinTheDocumentedUlpOfTheReference)
{
    // The documented bounds, asserted as they are written in math/DeterministicMath.hpp against the double-double
    // reference: 1 ulp (Sin, Cos, Atan), 2 (Exp, Log, Log2), 3 (Tan, Atan2, Asin, Acos)
    Sweep sweep(13);
    auto check = [](const char *name, const double x, const double actual, const DD reference, const double bound) {
        if (reference.hi == 0.0)
        {
            EXPECT_EQ(actual, 0.0) << name << "(" << x << ")";
            return;
        }
        EXPECT_LE(UlpErrorDD(actual, reference), bound) << name << "(" << x << ") = " << actual << ", reference " << reference.hi;
    };
    for (int i = 0; i < 20000; ++i)
    {
        const double x = sweep.Uniform(-1000.0, 1000.0);
        check("Sin", x, Math::Sin(x), RefSin(x), 1.0);
        check("Cos", x, Math::Cos(x), RefCos(x), 1.0);
        check("Tan", x, Math::Tan(x), RefTan(x), 3.0);
        const double big = sweep.Uniform(-1.0e6, 1.0e6); // under 2^20, so still the accurate reduction
        check("Sin(big)", big, Math::Sin(big), RefSin(big), 1.0);
        check("Cos(big)", big, Math::Cos(big), RefCos(big), 1.0);
        const double unit = sweep.Uniform(-1.0, 1.0);
        check("Asin", unit, Math::Asin(unit), RefAsin(unit), 3.0);
        check("Acos", unit, Math::Acos(unit), RefAcos(unit), 3.0);
        const double wide = std::tan(sweep.Uniform(-1.5707, 1.5707));
        check("Atan", wide, Math::Atan(wide), RefAtan(wide), 1.0);
        const double e = sweep.Uniform(-700.0, 700.0);
        check("Exp", e, Math::Exp(e), RefExp(e), 2.0);
        const double positive = std::ldexp(sweep.Uniform(1.0, 2.0), static_cast<int>(sweep.Uniform(-1000.0, 1000.0)));
        check("Log", positive, Math::Log(positive), RefLog(positive), 2.0);
        check("Log2", positive, Math::Log2(positive), RefLog2(positive), 2.0);
        const double y = sweep.Uniform(-1000.0, 1000.0);
        const double xx = sweep.Uniform(-1000.0, 1000.0);
        check("Atan2", y, Math::Atan2(y, xx), RefAtan2(y, xx), 3.0);
    }
}

// Near the multiples of pi/2 the reduction must keep its relative accuracy (the result is tiny there): doubles that are as
// close to a multiple as a double can be, up to the 2^20 limit
TEST(DeterministicMathTest, SineAndCosineStayAccurateNextToMultiplesOfHalfPi)
{
    for (int k = 1; k < 5000; ++k)
    {
        const double x = static_cast<double>(k) * (std::numbers::pi / 2.0); // the double nearest, not the multiple itself
        const double sine = Math::Sin(x);
        const double cosine = Math::Cos(x);
        if (std::fabs(sine) < 1e-3)
        {
            EXPECT_LE(UlpErrorDD(sine, RefSin(x)), 1.0) << "Sin(" << x << ")";
        }
        if (std::fabs(cosine) < 1e-3)
        {
            EXPECT_LE(UlpErrorDD(cosine, RefCos(x)), 1.0) << "Cos(" << x << ")";
        }
    }
    // The classic values: the double nearest pi has a sine of 1.2246467991473532e-16, the double nearest pi/2 a cosine of
    // 6.123233995736766e-17 (neither is 0: the doubles are not the real pi)
    EXPECT_NEAR(Math::Sin(std::numbers::pi) / 1.2246467991473532e-16, 1.0, 1e-14);
    EXPECT_NEAR(Math::Cos(std::numbers::pi / 2.0) / 6.123233995736766e-17, 1.0, 1e-14);
    EXPECT_EQ(Math::Sin(std::numbers::pi / 2.0), 1.0);
    EXPECT_EQ(Math::Cos(0.0), 1.0);
}

// Beyond 2^20 the argument is first reduced with fmod(x, 2*pi): defined and the same everywhere, accurate to about
// |x| * 4e-17, and continuous across the limit
TEST(DeterministicMathTest, ArgumentsBeyondTheReductionLimitAreDefinedAndContinuous)
{
    const double limit = 1048576.0;
    EXPECT_NEAR(Math::Sin(limit), Math::Sin(std::nextafter(limit, 2.0e6)), 1e-9);
    EXPECT_NEAR(Math::Sin(std::nextafter(limit, 2.0e6)), std::sin(std::nextafter(limit, 2.0e6)), 1e-9);
    for (const double x : {2.0e6, 1.0e9, 1.0e12, 123456789012.0, 1.0e15})
    {
        EXPECT_NEAR(Math::Sin(x), std::sin(x), std::fabs(x) * 1e-16 + 1e-12) << x;
        EXPECT_NEAR(Math::Cos(x), std::cos(x), std::fabs(x) * 1e-16 + 1e-12) << x;
        EXPECT_EQ(Bits(Math::Sin(x)), Bits(Math::Sin(x))) << "the same call gives the same bits";
    }
    // Finite however large
    EXPECT_FALSE(std::isnan(Math::Sin(1.0e300)));
    EXPECT_LE(std::fabs(Math::Sin(1.0e300)), 1.0);
    EXPECT_LE(std::fabs(Math::Cos(-1.7976931348623157e308)), 1.0);
}

// -- special values --------------------------------------------------------------------------------------------------

TEST(DeterministicMathTest, NaNInGivesTheCanonicalNaNOut)
{
    // A NaN with a payload and the sign bit set (what x86 hardware makes for 0/0) must not pass through
    const double inputs[] = {std::numeric_limits<double>::quiet_NaN(), DoubleOf(0xFFF8000000000123ull), DoubleOf(0x7FF0000000000001ull)};
    for (const double nan : inputs)
    {
        ASSERT_TRUE(std::isnan(nan));
        EXPECT_EQ(Bits(Math::Sin(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Cos(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Tan(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Asin(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Acos(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Atan(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Atan2(nan, 1.0)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Atan2(1.0, nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Exp(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Log(nan)), kCanonicalNaN);
        EXPECT_EQ(Bits(Math::Log2(nan)), kCanonicalNaN);
    }
    const float floatNaNs[] = {std::numeric_limits<float>::quiet_NaN(), FloatOf(0xFFC00123u)};
    for (const float nan : floatNaNs)
    {
        ASSERT_TRUE(std::isnan(nan));
        EXPECT_EQ(Bits(Math::Sin(nan)), kCanonicalNaNf);
        EXPECT_EQ(Bits(Math::Atan2(nan, nan)), kCanonicalNaNf);
        EXPECT_EQ(Bits(Math::Exp(nan)), kCanonicalNaNf);
        EXPECT_EQ(Bits(Math::Log(nan)), kCanonicalNaNf);
    }
}

TEST(DeterministicMathTest, DomainErrorsGiveTheCanonicalNaN)
{
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(Bits(Math::Sin(inf)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Cos(-inf)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Tan(inf)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Asin(1.0000001)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Asin(-2.0)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Acos(1.0000001)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Acos(inf)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Log(-1.0)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Log(-inf)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Log2(-0.5)), kCanonicalNaN);
    EXPECT_EQ(Bits(Math::Sin(std::numeric_limits<float>::infinity())), kCanonicalNaNf);
    EXPECT_EQ(Bits(Math::Acos(2.0f)), kCanonicalNaNf);
    EXPECT_EQ(Bits(Math::Log(-1.0f)), kCanonicalNaNf);
}

TEST(DeterministicMathTest, ZerosKeepTheirSignAndTheEdgeValuesAreExact)
{
    EXPECT_EQ(Bits(Math::Sin(0.0)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Sin(-0.0)), Bits(-0.0));
    EXPECT_EQ(Bits(Math::Tan(-0.0)), Bits(-0.0));
    EXPECT_EQ(Bits(Math::Atan(-0.0)), Bits(-0.0));
    EXPECT_EQ(Bits(Math::Asin(-0.0)), Bits(-0.0));
    EXPECT_EQ(Bits(Math::Sin(-0.0f)), Bits(-0.0f));
    EXPECT_EQ(Bits(Math::Cos(0.0)), Bits(1.0));
    EXPECT_EQ(Bits(Math::Cos(-0.0)), Bits(1.0));
    EXPECT_EQ(Bits(Math::Acos(1.0)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Acos(-1.0)), Bits(std::numbers::pi));
    EXPECT_EQ(Bits(Math::Asin(1.0)), Bits(std::numbers::pi / 2.0));
    EXPECT_EQ(Bits(Math::Asin(-1.0)), Bits(-std::numbers::pi / 2.0));
    EXPECT_EQ(Bits(Math::Acos(0.0)), Bits(std::numbers::pi / 2.0));
    EXPECT_EQ(Bits(Math::Atan(1.0)), Bits(std::numbers::pi / 4.0));
    EXPECT_EQ(Bits(Math::Atan(std::numeric_limits<double>::infinity())), Bits(std::numbers::pi / 2.0));
    EXPECT_EQ(Bits(Math::Atan(-std::numeric_limits<double>::infinity())), Bits(-std::numbers::pi / 2.0));
    EXPECT_EQ(Bits(Math::Exp(0.0)), Bits(1.0));
    EXPECT_EQ(Bits(Math::Log(1.0)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Log2(1.0)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Exp(1.0f)), Bits(std::numbers::e_v<float>));
    // Tiny arguments: the function is the argument itself (sin x = x - x^3/6 rounds to x), also for denormals
    EXPECT_EQ(Math::Sin(1e-10), 1e-10);
    EXPECT_EQ(Math::Tan(-3e-12), -3e-12);
    EXPECT_EQ(Math::Atan(5e-324), 5e-324);
    EXPECT_EQ(Math::Sin(5e-324), 5e-324);
    EXPECT_EQ(Math::Cos(1e-10), 1.0);
}

TEST(DeterministicMathTest, Atan2FollowsTheCTable)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double pi = std::numbers::pi;
    struct Case
    {
        double y;
        double x;
        double expected;
    };
    const Case cases[] = {
        {0.0, 1.0, 0.0},       {-0.0, 1.0, -0.0},      {0.0, -1.0, pi},        {-0.0, -1.0, -pi},
        {0.0, 0.0, 0.0},       {-0.0, 0.0, -0.0},      {0.0, -0.0, pi},        {-0.0, -0.0, -pi},
        {1.0, 0.0, pi / 2},    {-1.0, 0.0, -pi / 2},   {1.0, -0.0, pi / 2},    {-1.0, -0.0, -pi / 2},
        {inf, inf, pi / 4},    {-inf, inf, -pi / 4},   {inf, -inf, 3 * pi / 4}, {-inf, -inf, -3 * pi / 4},
        {1.0, inf, 0.0},       {-1.0, inf, -0.0},      {1.0, -inf, pi},        {-1.0, -inf, -pi},
        {inf, 1.0, pi / 2},    {-inf, -1.0, -pi / 2},  {1.0, 1.0, pi / 4},     {-1.0, -1.0, -3 * pi / 4},
    };
    for (const Case &c : cases)
    {
        EXPECT_LE(UlpError(Math::Atan2(c.y, c.x), c.expected), 1.0) << "Atan2(" << c.y << ", " << c.x << ")";
        EXPECT_EQ(std::signbit(Math::Atan2(c.y, c.x)), std::signbit(c.expected)) << "the sign of Atan2(" << c.y << ", " << c.x << ")";
    }
    // The ones that are exactly a constant
    EXPECT_EQ(Bits(Math::Atan2(0.0, -1.0)), Bits(std::numbers::pi));
    EXPECT_EQ(Bits(Math::Atan2(7.0, 7.0)), Bits(std::numbers::pi / 4.0));
    EXPECT_EQ(Bits(Math::Atan2(inf, -inf)), Bits(3.0 * std::numbers::pi / 4.0));
    // A ratio that underflows or overflows still answers
    EXPECT_EQ(Math::Atan2(1e-300, 1e300), 0.0);
    EXPECT_EQ(Bits(Math::Atan2(1e-300, -1e300)), Bits(std::numbers::pi));
    EXPECT_EQ(Bits(Math::Atan2(1e300, 1e-300)), Bits(std::numbers::pi / 2.0));
    EXPECT_EQ(Math::Atan2(1e-310, 1.0), 1e-310);
}

TEST(DeterministicMathTest, ExpAndLogCoverTheWholeRange)
{
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(Bits(Math::Exp(inf)), Bits(inf));
    EXPECT_EQ(Bits(Math::Exp(-inf)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Exp(710.5)), Bits(inf));
    EXPECT_EQ(Bits(Math::Exp(1.0e6)), Bits(inf));
    EXPECT_EQ(Bits(Math::Exp(-800.0)), Bits(0.0));
    EXPECT_EQ(Bits(Math::Exp(-1.0e6)), Bits(0.0));
    // Just under the overflow limit and the underflow through the denormals (no flush: the guard keeps FTZ off)
    EXPECT_LE(UlpErrorDD(Math::Exp(709.78), RefExp(709.78)), 2.0);
    EXPECT_TRUE(std::isfinite(Math::Exp(709.78)));
    const double denormal = Math::Exp(-740.0);
    EXPECT_GT(denormal, 0.0);
    EXPECT_LT(denormal, std::numeric_limits<double>::min());
    EXPECT_NEAR(denormal / std::exp(-740.0), 1.0, 1e-9);
    EXPECT_EQ(Bits(Math::Exp(-745.1)), Bits(5e-324));
    EXPECT_EQ(Bits(Math::Exp(1.0f)), Bits(static_cast<float>(std::numbers::e)));
    EXPECT_EQ(Bits(Math::Exp(100.0f)), Bits(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(Bits(Math::Exp(-110.0f)), Bits(0.0f));

    EXPECT_EQ(Bits(Math::Log(0.0)), Bits(-inf));
    EXPECT_EQ(Bits(Math::Log(-0.0)), Bits(-inf));
    EXPECT_EQ(Bits(Math::Log(inf)), Bits(inf));
    EXPECT_EQ(Bits(Math::Log2(0.0)), Bits(-inf));
    EXPECT_EQ(Bits(Math::Log2(inf)), Bits(inf));
    // A denormal: Log keeps its accuracy there
    EXPECT_LE(UlpErrorDD(Math::Log(5e-324), RefLog(5e-324)), 2.0);
    EXPECT_LE(UlpErrorDD(Math::Log(1e-310), RefLog(1e-310)), 2.0);
    EXPECT_LE(UlpErrorDD(Math::Log(1.7976931348623157e308), RefLog(1.7976931348623157e308)), 2.0);
}

TEST(DeterministicMathTest, Log2OfAPowerOfTwoIsExact)
{
    for (int k = -1074; k <= 1023; ++k)
    {
        const double power = std::ldexp(1.0, k);
        EXPECT_EQ(Math::Log2(power), static_cast<double>(k)) << "Log2(2^" << k << ")";
    }
    EXPECT_EQ(Math::Log2(8.0f), 3.0f);
    EXPECT_EQ(Math::Log2(0.25f), -2.0f);
}

// -- structure -------------------------------------------------------------------------------------------------------

TEST(DeterministicMathTest, OddAndEvenFunctionsAreExactlySymmetric)
{
    Sweep sweep(17);
    for (int i = 0; i < 5000; ++i)
    {
        const double x = sweep.Uniform(0.0, 5000.0);
        EXPECT_EQ(Bits(Math::Sin(-x)), Bits(-Math::Sin(x))) << x;
        EXPECT_EQ(Bits(Math::Cos(-x)), Bits(Math::Cos(x))) << x;
        EXPECT_EQ(Bits(Math::Tan(-x)), Bits(-Math::Tan(x))) << x;
        EXPECT_EQ(Bits(Math::Atan(-x)), Bits(-Math::Atan(x))) << x;
        const double unit = sweep.Unit();
        EXPECT_EQ(Bits(Math::Asin(-unit)), Bits(-Math::Asin(unit))) << unit;
    }
}

TEST(DeterministicMathTest, SineAndCosineSatisfyThePythagoreanIdentityAndPeriodicity)
{
    Sweep sweep(19);
    for (int i = 0; i < 5000; ++i)
    {
        const double x = sweep.Uniform(-500.0, 500.0);
        const double s = Math::Sin(x);
        const double c = Math::Cos(x);
        EXPECT_NEAR(s * s + c * c, 1.0, 1e-15) << x;
        EXPECT_NEAR(Math::Sin(x + 2.0 * std::numbers::pi), s, 1e-12) << x;
        EXPECT_NEAR(Math::Cos(x + 2.0 * std::numbers::pi), c, 1e-12) << x;
        EXPECT_NEAR(Math::Sin(x + std::numbers::pi / 2.0), c, 1e-12) << x;
        const double t = Math::Tan(x);
        if (std::fabs(c) > 1e-3)
        {
            EXPECT_NEAR(t, s / c, std::fabs(t) * 1e-14 + 1e-15) << x;
        }
    }
}

TEST(DeterministicMathTest, TheFunctionsAreMonotoneWhereTheMathIs)
{
    // Sin rises on [-pi/2, pi/2], Cos falls on [0, pi], Tan rises on (-pi/2, pi/2), Atan, Asin, Exp and Log rise, Acos falls
    double previousSin = -2.0, previousCos = 2.0, previousTan = -1e300, previousAtan = -2.0, previousAsin = -2.0;
    double previousAcos = 4.0, previousExp = -1.0, previousLog = -1e300;
    const int steps = 20000;
    for (int i = 0; i <= steps; ++i)
    {
        const double t = static_cast<double>(i) / steps;
        const double sine = Math::Sin(-1.5707 + 3.1414 * t);
        EXPECT_GE(sine, previousSin) << i;
        previousSin = sine;
        const double cosine = Math::Cos(3.14159 * t);
        EXPECT_LE(cosine, previousCos) << i;
        previousCos = cosine;
        const double tangent = Math::Tan(-1.5707 + 3.1414 * t);
        EXPECT_GE(tangent, previousTan) << i;
        previousTan = tangent;
        const double arctangent = Math::Atan(-50.0 + 100.0 * t);
        EXPECT_GE(arctangent, previousAtan) << i;
        previousAtan = arctangent;
        const double arcsine = Math::Asin(-1.0 + 2.0 * t);
        EXPECT_GE(arcsine, previousAsin) << i;
        previousAsin = arcsine;
        const double arccosine = Math::Acos(-1.0 + 2.0 * t);
        EXPECT_LE(arccosine, previousAcos) << i;
        previousAcos = arccosine;
        const double exponential = Math::Exp(-30.0 + 60.0 * t);
        EXPECT_GE(exponential, previousExp) << i;
        previousExp = exponential;
        const double logarithm = Math::Log(1e-3 + 1000.0 * t);
        EXPECT_GE(logarithm, previousLog) << i;
        previousLog = logarithm;
    }
}

TEST(DeterministicMathTest, TheInverseFunctionsUndoTheirFunctions)
{
    Sweep sweep(23);
    for (int i = 0; i < 5000; ++i)
    {
        const double x = sweep.Uniform(-1.5, 1.5);
        EXPECT_NEAR(Math::Asin(Math::Sin(x)), x, 1e-14) << x;
        EXPECT_NEAR(Math::Atan(Math::Tan(x)), x, 1e-13) << x;
        // Away from 0 and pi, where acos is ill-conditioned (its slope is infinite there)
        const double angle = sweep.Uniform(0.3, 2.8);
        EXPECT_NEAR(Math::Acos(Math::Cos(angle)), angle, 1e-14) << angle;
        const double e = sweep.Uniform(-100.0, 100.0);
        EXPECT_NEAR(Math::Log(Math::Exp(e)), e, 1e-13 * std::max(1.0, std::fabs(e))) << e;
        const double direction = sweep.Uniform(-3.1, 3.1);
        EXPECT_NEAR(Math::Atan2(Math::Sin(direction), Math::Cos(direction)), direction, 1e-14) << direction;
    }
}

// -- the call sites that used the C library ---------------------------------------------------------------------------

TEST(DeterministicMathCallSitesTest, RotationMatricesUseTheEngineOwnedFunctions)
{
    using Matrix4 = Math::Matrix<float, 4, 4>;
    using Matrix3 = Math::Matrix<float, 3, 3>;
    using Matrix2 = Math::Matrix<float, 2, 2>;
    for (const float angle : {0.1f, 0.7f, 1.3f, 2.9f, -0.6f, 10.0f})
    {
        const float c = Math::Cos(angle);
        const float s = Math::Sin(angle);
        const Matrix4 x4 = Matrix4::RotationX(angle);
        EXPECT_EQ(Bits(x4(1, 1)), Bits(c));
        EXPECT_EQ(Bits(x4(2, 1)), Bits(s));
        const Matrix4 y4 = Matrix4::RotationY(angle);
        EXPECT_EQ(Bits(y4(0, 0)), Bits(c));
        EXPECT_EQ(Bits(y4(0, 2)), Bits(s));
        const Matrix4 z4 = Matrix4::RotationZ(angle);
        EXPECT_EQ(Bits(z4(0, 0)), Bits(c));
        EXPECT_EQ(Bits(z4(1, 0)), Bits(s));
        const Matrix3 x3 = Matrix3::RotationX(angle);
        EXPECT_EQ(Bits(x3(1, 1)), Bits(c));
        EXPECT_EQ(Bits(x3(2, 1)), Bits(s));
        const Matrix3 z3 = Matrix3::RotationZ(angle);
        EXPECT_EQ(Bits(z3(0, 0)), Bits(c));
        EXPECT_EQ(Bits(z3(1, 0)), Bits(s));
        const Matrix2 r2 = Matrix2::Rotation(angle);
        EXPECT_EQ(Bits(r2(0, 0)), Bits(c));
        EXPECT_EQ(Bits(r2(1, 0)), Bits(s));
    }
}

TEST(DeterministicMathCallSitesTest, QuaternionConstructionAndConversionUseTheEngineOwnedFunctions)
{
    for (const float angle : {0.3f, 1.1f, 2.5f, -0.9f})
    {
        const float half = angle * 0.5f;
        const Math::Quaternion axisAngle(Math::Vector3(0.0f, 1.0f, 0.0f), angle);
        EXPECT_EQ(Bits(axisAngle.GetW()), Bits(Math::Cos(half)));
        EXPECT_EQ(Bits(axisAngle.GetY()), Bits(Math::Sin(half)));
        // Euler (pitch, yaw, roll) with only the yaw set: w = cos(yaw / 2), y = sin(yaw / 2)
        const Math::Quaternion euler(0.0f, angle, 0.0f);
        EXPECT_EQ(Bits(euler.GetW()), Bits(Math::Cos(half)));
        EXPECT_EQ(Bits(euler.GetY()), Bits(Math::Sin(half)));
        // The angle between a rotation and the identity: 2 * acos(|w|)
        const float angleBetween = Math::Quaternion::Identity.Angle(axisAngle);
        EXPECT_EQ(Bits(angleBetween), Bits(Math::Acos(std::min(std::fabs(axisAngle.GetW()), 1.0f)) * 2.0f));
        // ToEulerAngles: yaw is asin(2 (w y - z x)), pitch and roll atan2
        const Math::Vector3 back = axisAngle.ToEulerAngles();
        const float siny = 2.0f * (axisAngle.GetW() * axisAngle.GetY() - axisAngle.GetZ() * axisAngle.GetX());
        if (std::fabs(siny) < 1.0f)
        {
            EXPECT_EQ(Bits(back.y), Bits(Math::Asin(siny)));
        }
        EXPECT_EQ(Bits(back.x), Bits(Math::Atan2(2.0f * (axisAngle.GetW() * axisAngle.GetX() + axisAngle.GetY() * axisAngle.GetZ()),
                                                 1.0f - 2.0f * (axisAngle.GetX() * axisAngle.GetX() + axisAngle.GetY() * axisAngle.GetY()))));
    }
}

TEST(DeterministicMathCallSitesTest, VectorAnglesAndSlerpUseTheEngineOwnedFunctions)
{
    const Math::Vector2 v(3.0f, 4.0f);
    EXPECT_EQ(Bits(v.Angle()), Bits(Math::Atan2(4.0f, 3.0f)));
    const Math::Vector2 turned = Math::Vector2::FromAngle(0.8f);
    EXPECT_EQ(Bits(turned.x), Bits(Math::Cos(0.8f)));
    EXPECT_EQ(Bits(turned.y), Bits(Math::Sin(0.8f)));

    const Math::Vector3 a(1.0f, 0.0f, 0.0f);
    const Math::Vector3 b(0.0f, 1.0f, 0.0f);
    EXPECT_EQ(Bits(a.AngleTo(b)), Bits(Math::Acos(0.0f)));
}

// The engine's random unit vectors on a circle use the engine-owned trig too (the unseeded one draws an angle)
TEST(DeterministicMathCallSitesTest, TheUnitCircleSampleIsAUnitVectorOfTheEngineOwnedFunctions)
{
    for (int i = 0; i < 50; ++i)
    {
        const Math::Vector2 on = Math::Random::RandomOnUnitCircle();
        EXPECT_NEAR(on.x * on.x + on.y * on.y, 1.0f, 1e-6f);
    }
}
