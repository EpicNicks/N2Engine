#include "math/DeterministicMath.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The engine-owned transcendental functions (see the header for the contract). This file is compiled with
// floating-point contraction off (math/CMakeLists.txt): everything below is a fixed sequence of IEEE-754 double +, -, *,
// / and sqrt, each correctly rounded, so the result is the same on every target. Nothing here calls a C library
// transcendental function; fmod (exact by definition), sqrt (correctly rounded), fabs, signbit and the classification
// functions are the only <cmath> calls, and none of them has a last-bit variation between platforms.

namespace N2Engine::Math
{
    namespace
    {
        constexpr double FromBits(const std::uint64_t bits)
        {
            return std::bit_cast<double>(bits);
        }

        // The one NaN every function returns for a NaN or a domain error (the sign and payload of a NaN made by hardware
        // differ between x86 and arm)
        constexpr double kNaN = FromBits(0x7FF8000000000000ull);
        constexpr double kInf = FromBits(0x7FF0000000000000ull);
        constexpr float kNaNf = std::bit_cast<float>(0x7FC00000u);

        // pi/2 as three 33-bit pieces and a tail (each piece times an integer below 2^20 is exact in a double)
        constexpr double kPio2_1 = FromBits(0x3FF921FB54400000ull);  // 1.5707963267341256
        constexpr double kPio2_2 = FromBits(0x3DD0B4611A600000ull);  // 6.077100506303966e-11
        constexpr double kPio2_3 = FromBits(0x3BA3198A2E000000ull);  // 2.0222662487111665e-21
        constexpr double kPio2_3t = FromBits(0x397B839A252049C1ull); // 8.4784276603689e-32
        constexpr double kInvPio2 = FromBits(0x3FE45F306DC9C883ull); // 2/pi
        constexpr double kTwoPi = FromBits(0x401921FB54442D18ull);   // the double nearest 2*pi
        constexpr double kPio4 = FromBits(0x3FE921FB54442D18ull);
        constexpr double k3Pio4 = FromBits(0x4002D97C7F3321D2ull);
        // pi and pi/2 as a double and the double that is the rest of them
        constexpr double kPiHi = FromBits(0x400921FB54442D18ull);
        constexpr double kPiLo = FromBits(0x3CA1A62633145C07ull);
        constexpr double kPio2Hi = FromBits(0x3FF921FB54442D18ull);
        constexpr double kPio2Lo = FromBits(0x3C91A62633145C07ull);
        // ln 2 as a 32-bit piece (k * piece is exact for |k| < 2^21) and the rest
        constexpr double kLn2Hi = FromBits(0x3FE62E42FEE00000ull);
        constexpr double kLn2Lo = FromBits(0x3DEA39EF35793C76ull);
        constexpr double kInvLn2 = FromBits(0x3FF71547652B82FEull);
        // 1/ln2 as a 32-bit piece and the rest (Log2)
        constexpr double kInvLn2Hi = FromBits(0x3FF7154765200000ull);
        constexpr double kInvLn2Lo = FromBits(0x3DE705FC2EEFA200ull);
        constexpr double kSqrt2 = FromBits(0x3FF6A09E667F3BCDull);

        constexpr double kReduceLimit = 1048576.0;                     // 2^20: Cody-Waite reduction is exact up to here
        constexpr double kTiny = FromBits(0x3E40000000000000ull);      // 2^-27: sin x = x, cos x = 1, atan x = x, tan x = x
        constexpr double kMinNormal = FromBits(0x0010000000000000ull); // 2^-1022
        constexpr double kTwo54 = FromBits(0x4350000000000000ull);

        // sin r = r + r*z*P(z), z = r*r: P = -1/3!, 1/5!, -1/7!, ... (to r^23)
        constexpr double kSinP[] = {
            FromBits(0xBFC5555555555555ull), // -0.16666666666666666
            FromBits(0x3F81111111111111ull), // 0.008333333333333333
            FromBits(0xBF2A01A01A01A01Aull), // -0.0001984126984126984
            FromBits(0x3EC71DE3A556C734ull), // 2.7557319223985893e-06
            FromBits(0xBE5AE64567F544E4ull), // -2.505210838544172e-08
            FromBits(0x3DE6124613A86D09ull), // 1.6059043836821613e-10
            FromBits(0xBD6AE7F3E733B81Full), // -7.647163731819816e-13
            FromBits(0x3CE952C77030AD4Aull), // 2.8114572543455206e-15
            FromBits(0xBC62F49B46814157ull), // -8.22063524662433e-18
            FromBits(0x3BD71B8EF6DCF572ull), // 1.9572941063391263e-20
            FromBits(0xBB4761B41316381Aull) // -3.868170170630684e-23
        };
        // cos r = (1 - z/2) + z*z*P(z): P = 1/4!, -1/6!, 1/8!, ... (to r^24)
        constexpr double kCosP[] = {
            FromBits(0x3FA5555555555555ull), // 0.041666666666666664
            FromBits(0xBF56C16C16C16C17ull), // -0.001388888888888889
            FromBits(0x3EFA01A01A01A01Aull), // 2.48015873015873e-05
            FromBits(0xBE927E4FB7789F5Cull), // -2.755731922398589e-07
            FromBits(0x3E21EED8EFF8D898ull), // 2.08767569878681e-09
            FromBits(0xBDA93974A8C07C9Dull), // -1.1470745597729725e-11
            FromBits(0x3D2AE7F3E733B81Full), // 4.779477332387385e-14
            FromBits(0xBCA6827863B97D97ull), // -1.5619206968586225e-16
            FromBits(0x3C1E542BA4020225ull), // 4.110317623312165e-19
            FromBits(0xBB90CE396DB7F853ull), // -8.896791392450574e-22
            FromBits(0x3AFF2CF01972F578ull) // 1.6117375710961184e-24
        };
        // exp r = (1 + r) + r*r*P(r): P = 1/2!, 1/3!, ... 1/14! (Horner in r)
        constexpr double kExpP[] = {
            FromBits(0x3FE0000000000000ull), // 0.5
            FromBits(0x3FC5555555555555ull), // 0.16666666666666666
            FromBits(0x3FA5555555555555ull), // 0.041666666666666664
            FromBits(0x3F81111111111111ull), // 0.008333333333333333
            FromBits(0x3F56C16C16C16C17ull), // 0.001388888888888889
            FromBits(0x3F2A01A01A01A01Aull), // 0.0001984126984126984
            FromBits(0x3EFA01A01A01A01Aull), // 2.48015873015873e-05
            FromBits(0x3EC71DE3A556C734ull), // 2.7557319223985893e-06
            FromBits(0x3E927E4FB7789F5Cull), // 2.755731922398589e-07
            FromBits(0x3E5AE64567F544E4ull), // 2.505210838544172e-08
            FromBits(0x3E21EED8EFF8D898ull), // 2.08767569878681e-09
            FromBits(0x3DE6124613A86D09ull), // 1.6059043836821613e-10
            FromBits(0x3DA93974A8C07C9Dull) // 1.1470745597729725e-11
        };
        // atan t = t + t*z*P(z), z = t*t: P = -1/3, 1/5, -1/7, ... (to t^45; |t| < 7/16 leaves under 1e-18 relative)
        constexpr double kAtanP[] = {
            FromBits(0xBFD5555555555555ull), // -0.3333333333333333
            FromBits(0x3FC999999999999Aull), // 0.2
            FromBits(0xBFC2492492492492ull), // -0.14285714285714285
            FromBits(0x3FBC71C71C71C71Cull), // 0.1111111111111111
            FromBits(0xBFB745D1745D1746ull), // -0.09090909090909091
            FromBits(0x3FB3B13B13B13B14ull), // 0.07692307692307693
            FromBits(0xBFB1111111111111ull), // -0.06666666666666667
            FromBits(0x3FAE1E1E1E1E1E1Eull), // 0.058823529411764705
            FromBits(0xBFAAF286BCA1AF28ull), // -0.05263157894736842
            FromBits(0x3FA8618618618618ull), // 0.047619047619047616
            FromBits(0xBFA642C8590B2164ull), // -0.043478260869565216
            FromBits(0x3FA47AE147AE147Bull), // 0.04
            FromBits(0xBFA2F684BDA12F68ull), // -0.037037037037037035
            FromBits(0x3FA1A7B9611A7B96ull), // 0.034482758620689655
            FromBits(0xBFA0842108421084ull), // -0.03225806451612903
            FromBits(0x3F9F07C1F07C1F08ull), // 0.030303030303030304
            FromBits(0xBF9D41D41D41D41Dull), // -0.02857142857142857
            FromBits(0x3F9BACF914C1BAD0ull), // 0.02702702702702703
            FromBits(0xBF9A41A41A41A41Aull), // -0.02564102564102564
            FromBits(0x3F98F9C18F9C18FAull), // 0.024390243902439025
            FromBits(0xBF97D05F417D05F4ull), // -0.023255813953488372
            FromBits(0x3F96C16C16C16C17ull) // 0.022222222222222223
        };
        // atan c for the reduction points c = 1/2, 1, 3/2, as the double nearest and the double that is the rest
        constexpr double kAtanHalfHi = FromBits(0x3FDDAC670561BB4Full);
        constexpr double kAtanHalfLo = FromBits(0x3C7A2B7F222F65E2ull);
        constexpr double kAtanOneHi = FromBits(0x3FE921FB54442D18ull); // pi/4
        constexpr double kAtanOneLo = FromBits(0x3C81A62633145C07ull);
        constexpr double kAtanThreeHalvesHi = FromBits(0x3FEF730BD281F69Bull);
        constexpr double kAtanThreeHalvesLo = FromBits(0x3C7007887AF0CBBDull);
        // log m = 2s + 2s*z*P(z), s = (m-1)/(m+1), z = s*s: P = 1/3, 1/5, ... (to s^25)
        constexpr double kLogP[] = {
            FromBits(0x3FD5555555555555ull), // 0.3333333333333333
            FromBits(0x3FC999999999999Aull), // 0.2
            FromBits(0x3FC2492492492492ull), // 0.14285714285714285
            FromBits(0x3FBC71C71C71C71Cull), // 0.1111111111111111
            FromBits(0x3FB745D1745D1746ull), // 0.09090909090909091
            FromBits(0x3FB3B13B13B13B14ull), // 0.07692307692307693
            FromBits(0x3FB1111111111111ull), // 0.06666666666666667
            FromBits(0x3FAE1E1E1E1E1E1Eull), // 0.058823529411764705
            FromBits(0x3FAAF286BCA1AF28ull), // 0.05263157894736842
            FromBits(0x3FA8618618618618ull), // 0.047619047619047616
            FromBits(0x3FA642C8590B2164ull), // 0.043478260869565216
            FromBits(0x3FA47AE147AE147Bull) // 0.04
        };

        // c[0] + z * (c[1] + z * (c[2] + ...)), evaluated from the last coefficient: a fixed order
        template <std::size_t N>
        double Horner(const double (&c)[N], const double z)
        {
            double p = c[N - 1];
            for (std::size_t i = N - 1; i-- > 0;)
            {
                p = c[i] + z * p;
            }
            return p;
        }

        // 2^e for e in [-1022, 1023]
        double TwoPow(const int e)
        {
            return FromBits(static_cast<std::uint64_t>(e + 1023) << 52);
        }

        // Round to nearest, ties away from zero, as an integer: truncation of an exact sum is exact
        std::int64_t RoundToInt(const double v)
        {
            return static_cast<std::int64_t>(v < 0.0 ? v - 0.5 : v + 0.5);
        }

        // a + b as the rounded sum and its exact rounding error (Knuth's TwoSum: no condition on the magnitudes)
        struct Sum
        {
            double hi;
            double lo;
        };

        Sum TwoSum(const double a, const double b)
        {
            const double s = a + b;
            const double bb = s - a;
            return {s, (a - (s - bb)) + (b - bb)};
        }

        struct Reduced
        {
            double r;          // x - n * pi/2 as r + tail, |r| <= about pi/4
            double tail;       // the part of the reduced argument below r's last bit
            unsigned quadrant; // n mod 4
        };

        Reduced ReducePio2(double x)
        {
            if (std::fabs(x) <= kPio4)
            {
                return {x, 0.0, 0u};
            }
            if (std::fabs(x) > kReduceLimit)
            {
                x = std::fmod(x, kTwoPi); // exact; the result is |x| < 2*pi (see the header for the accuracy)
            }
            const std::int64_t n = RoundToInt(x * kInvPio2);
            const double dn = static_cast<double>(n);
            // dn times each piece is exact; x - dn * kPio2_1 is exact (the two are within a factor of 2), and the two
            // subtractions after it keep their rounding errors, so r + tail carries the reduced argument to far below r's
            // last bit (no reduction error reaches the result, also next to a multiple of pi/2)
            const double r1 = x - dn * kPio2_1;
            const Sum s1 = TwoSum(r1, -(dn * kPio2_2));
            const Sum s2 = TwoSum(s1.hi, -(dn * kPio2_3));
            const double tail = (s1.lo + s2.lo) - dn * kPio2_3t;
            const Sum r = TwoSum(s2.hi, tail);
            return {r.hi, r.lo, static_cast<unsigned>(n & 3)};
        }

        // sin(r + tail) = r + r^3 P(r^2) + tail (1 - r^2/2), the tail's higher terms being below the last bit
        double SinKernel(const double r, const double tail)
        {
            const double z = r * r;
            const double v = z * r;
            return r + ((v * Horner(kSinP, z) - (0.5 * z) * tail) + tail);
        }

        // cos(r + tail) = 1 - r^2/2 + r^4 P(r^2) - r tail. 1 - r^2/2 is rounded once to w and its rounding error,
        // ((1 - w) - r^2/2) (both steps exact), is added back with the small terms
        double CosKernel(const double r, const double tail)
        {
            const double z = r * r;
            const double hz = 0.5 * z;
            const double w = 1.0 - hz;
            return w + (((1.0 - w) - hz) + ((z * z) * Horner(kCosP, z) - r * tail));
        }

        // atan t = t + t^3 P(t^2) for |t| < 7/16
        double AtanSeries(const double t)
        {
            const double z = t * t;
            return t + (t * z) * Horner(kAtanP, z);
        }

        // atan of a >= 0 (a may be +inf), reduced to |t| < 7/16 around c = 0, 1/2, 1, 3/2 or infinity:
        // atan a = atan c + atan((a - c) / (1 + a c)), with atan c a double and the rest of it (the fdlibm intervals).
        // a - c (2a - 1 for c = 1/2) is exact in its interval, so the reduced argument has at most three roundings and
        // is small next to atan c
        double AtanPositive(const double a)
        {
            if (a < 0.4375)
            {
                if (a < kTiny)
                {
                    return a; // atan a = a to double precision
                }
                return AtanSeries(a);
            }
            if (a < 0.6875)
            {
                return kAtanHalfHi + (kAtanHalfLo + AtanSeries((2.0 * a - 1.0) / (2.0 + a)));
            }
            if (a < 1.1875)
            {
                // a = 1 gives t = 0 and exactly kAtanOneHi, the double nearest pi/4, so Atan2(y, y) is that double
                return kAtanOneHi + (kAtanOneLo + AtanSeries((a - 1.0) / (a + 1.0)));
            }
            if (a < 2.4375)
            {
                return kAtanThreeHalvesHi + (kAtanThreeHalvesLo + AtanSeries((a - 1.5) / (1.0 + 1.5 * a)));
            }
            // atan a = pi/2 - atan(1/a); a = +inf gives t = -0, a series of 0 and exactly kPio2Hi
            return kPio2Hi + (kPio2Lo + AtanSeries(-1.0 / a));
        }

        // Splits a positive finite x into m * 2^e with m in [1/sqrt2, sqrt2]
        void SplitMantissa(double x, double &m, int &e)
        {
            e = 0;
            if (x < kMinNormal)
            {
                x *= kTwo54;
                e = -54;
            }
            const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
            e += static_cast<int>((bits >> 52) & 0x7FF) - 1023;
            m = std::bit_cast<double>((bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
            if (m > kSqrt2)
            {
                m *= 0.5;
                e += 1;
            }
        }

        // log m for m = 1 + f in [1/sqrt2, sqrt2] (f = m - 1 is exact here), split the fdlibm way so the caller can add
        // the large part f last: log(1 + f) = f - hfsq + r, hfsq = f^2/2, r = s (hfsq + R), s = f / (2 + f) and
        // R = 2 z P(z) with z = s^2 (s R is the series of 2 atanh s = log(1 + f) after its first term 2s)
        struct LogParts
        {
            double f;
            double hfsq;
            double r;
        };

        LogParts LogMantissa(const double m)
        {
            const double f = m - 1.0;
            const double hfsq = 0.5 * f * f;
            const double s = f / (2.0 + f);
            const double z = s * s;
            const double bigR = (2.0 * z) * Horner(kLogP, z);
            return {f, hfsq, s * (hfsq + bigR)};
        }

        float ToFloat(const double v)
        {
            return v != v ? kNaNf : static_cast<float>(v);
        }

        // A multiply-add whose fused and separately rounded results differ, read through volatile so no compiler can
        // fold it
        double ProbeMultiplyAdd()
        {
            volatile double a = 1.0 + 1.0 / 134217728.0;   // 1 + 2^-27
            volatile double c = -(1.0 + 1.0 / 67108864.0); // -(1 + 2^-26)
            const double x = a;
            const double y = c;
            // x * x = 1 + 2^-26 + 2^-54: rounded alone it is 1 + 2^-26 and the sum is 0, fused the sum is 2^-54
            return x * x + y;
        }
    }

    double Sin(const double x)
    {
        if (!std::isfinite(x))
        {
            return kNaN;
        }
        if (std::fabs(x) < kTiny)
        {
            return x;
        }
        const Reduced red = ReducePio2(x);
        switch (red.quadrant)
        {
        case 0u:
            return SinKernel(red.r, red.tail);
        case 1u:
            return CosKernel(red.r, red.tail);
        case 2u:
            return -SinKernel(red.r, red.tail);
        default:
            return -CosKernel(red.r, red.tail);
        }
    }

    double Cos(const double x)
    {
        if (!std::isfinite(x))
        {
            return kNaN;
        }
        if (std::fabs(x) < kTiny)
        {
            return 1.0;
        }
        const Reduced red = ReducePio2(x);
        switch (red.quadrant)
        {
        case 0u:
            return CosKernel(red.r, red.tail);
        case 1u:
            return -SinKernel(red.r, red.tail);
        case 2u:
            return -CosKernel(red.r, red.tail);
        default:
            return SinKernel(red.r, red.tail);
        }
    }

    double Tan(const double x)
    {
        if (!std::isfinite(x))
        {
            return kNaN;
        }
        if (std::fabs(x) < kTiny)
        {
            return x;
        }
        const Reduced red = ReducePio2(x);
        const double s = SinKernel(red.r, red.tail);
        const double c = CosKernel(red.r, red.tail);
        return (red.quadrant & 1u) == 0u ? s / c : -(c / s);
    }

    double Atan(const double x)
    {
        if (x != x)
        {
            return kNaN;
        }
        const double a = std::fabs(x);
        if (a < kTiny)
        {
            return x;
        }
        const double result = AtanPositive(a);
        return x < 0.0 ? -result : result;
    }

    double Atan2(const double y, const double x)
    {
        if (x != x || y != y)
        {
            return kNaN;
        }
        const bool yNegative = std::signbit(y);
        const bool xNegative = std::signbit(x);
        double result;
        if (y == 0.0)
        {
            result = xNegative ? kPiHi : 0.0;
        }
        else if (std::isinf(y))
        {
            if (std::isinf(x))
            {
                result = xNegative ? k3Pio4 : kPio4;
            }
            else
            {
                result = kPio2Hi;
            }
        }
        else if (x == 0.0)
        {
            result = kPio2Hi;
        }
        else if (std::isinf(x))
        {
            result = xNegative ? kPiHi : 0.0;
        }
        else
        {
            const double ay = std::fabs(y);
            const double ax = std::fabs(x);
            const double base = ay <= ax ? AtanPositive(ay / ax) : (kPio2Hi - AtanPositive(ax / ay)) + kPio2Lo;
            result = xNegative ? (kPiHi - base) + kPiLo : base;
        }
        return yNegative ? -result : result;
    }

    double Asin(const double x)
    {
        if (!(std::fabs(x) <= 1.0))
        {
            return kNaN; // NaN and |x| > 1
        }
        return Atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
    }

    double Acos(const double x)
    {
        if (!(std::fabs(x) <= 1.0))
        {
            return kNaN;
        }
        return Atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
    }

    double Exp(const double x)
    {
        if (x != x)
        {
            return kNaN;
        }
        if (x > 710.0)
        {
            return kInf;
        }
        if (x < -746.0)
        {
            return 0.0;
        }
        const std::int64_t k = RoundToInt(x * kInvLn2);
        const double dk = static_cast<double>(k);
        const double r = (x - dk * kLn2Hi) - dk * kLn2Lo;
        const double e = (1.0 + r) + (r * r) * Horner(kExpP, r);
        const int ki = static_cast<int>(k);
        if (ki > 1023)
        {
            return (e * 2.0) * TwoPow(1023); // 2^1024 is not a double
        }
        if (ki < -1021)
        {
            // Two steps so the one rounding into the denormals is the last one
            return (e * TwoPow(ki + 1000)) * TwoPow(-1000);
        }
        return e * TwoPow(ki);
    }

    double Log(const double x)
    {
        if (x != x || x < 0.0)
        {
            return kNaN;
        }
        if (x == 0.0)
        {
            return -kInf;
        }
        if (std::isinf(x))
        {
            return kInf;
        }
        double m;
        int e;
        SplitMantissa(x, m, e);
        const double dk = static_cast<double>(e);
        const LogParts p = LogMantissa(m);
        // e ln2 + log m with the small terms summed first and the exact e * kLn2Hi and f last (fdlibm's e_log.c order)
        return dk * kLn2Hi - ((p.hfsq - (p.r + dk * kLn2Lo)) - p.f);
    }

    double Log2(const double x)
    {
        if (x != x || x < 0.0)
        {
            return kNaN;
        }
        if (x == 0.0)
        {
            return -kInf;
        }
        if (std::isinf(x))
        {
            return kInf;
        }
        double m;
        int e;
        SplitMantissa(x, m, e);
        const LogParts p = LogMantissa(m);
        // log m / ln2 with 1/ln2 as two doubles (fdlibm's e_log2.c): f - hfsq is cut to its top 21 bits (hi, so
        // hi * kInvLn2Hi is exact) and the rest goes to lo, then e is added with its rounding error kept
        const double hi = std::bit_cast<double>(std::bit_cast<std::uint64_t>(p.f - p.hfsq) & 0xFFFFFFFF00000000ull);
        const double lo = ((p.f - hi) - p.hfsq) + p.r;
        const double valueHi = hi * kInvLn2Hi;
        double valueLo = (lo + hi) * kInvLn2Lo + lo * kInvLn2Hi;
        const double y = static_cast<double>(e);
        const double w = y + valueHi;
        valueLo = valueLo + ((y - w) + valueHi);
        return valueLo + w;
    }

    float Sin(const float x) { return ToFloat(Sin(static_cast<double>(x))); }
    float Cos(const float x) { return ToFloat(Cos(static_cast<double>(x))); }
    float Tan(const float x) { return ToFloat(Tan(static_cast<double>(x))); }
    float Asin(const float x) { return ToFloat(Asin(static_cast<double>(x))); }
    float Acos(const float x) { return ToFloat(Acos(static_cast<double>(x))); }
    float Atan(const float x) { return ToFloat(Atan(static_cast<double>(x))); }
    float Atan2(const float y, const float x) { return ToFloat(Atan2(static_cast<double>(y), static_cast<double>(x))); }
    float Exp(const float x) { return ToFloat(Exp(static_cast<double>(x))); }
    float Log(const float x) { return ToFloat(Log(static_cast<double>(x))); }
    float Log2(const float x) { return ToFloat(Log2(static_cast<double>(x))); }

    bool IsContractionFree()
    {
        return ProbeMultiplyAdd() == 0.0;
    }
}
