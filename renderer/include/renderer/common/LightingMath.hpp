#pragma once

#include <algorithm>
#include <cmath>

namespace Renderer::Common
{
    /**
     * The scalar maths the lit shaders share. The OpenGL lit shader spells the same formulas in GLSL; the
     * software renderer calls these, so the two agree and tests can check the formulas without a GPU.
     */

    /// Below this, a spot light's inner and outer cone are taken as one edge (an inner angle at or past the
    /// outer one gives a hard edge at the outer angle). Also the divisor floor of the GLSL
    /// `max(cosInner - cosOuter, 0.0001)`.
    inline constexpr float kMinSpotConeDelta = 0.0001f;

    /**
     * How much of a spot light reaches a point, from the cone alone: 1 inside the inner cone, 0 outside the
     * outer cone and a linear ramp in the cosine of the angle between them. `cosTheta` is the cosine of the angle between the spot's axis and the direction from the light to
     * the point; the angles are half-angles in radians (SpotLightData::innerConeAngle, outerConeAngle).
     */
    [[nodiscard]] inline float SpotConeFactorFromCosines(const float cosTheta, const float cosInner, const float cosOuter)
    {
        const float delta = std::max(cosInner - cosOuter, kMinSpotConeDelta);
        return std::clamp((cosTheta - cosOuter) / delta, 0.0f, 1.0f);
    }

    /// SpotConeFactorFromCosines for cone half-angles in radians
    [[nodiscard]] inline float SpotConeFactor(const float cosTheta, const float innerConeAngle, const float outerConeAngle)
    {
        return SpotConeFactorFromCosines(cosTheta, std::cos(innerConeAngle), std::cos(outerConeAngle));
    }

    /// The sRGB transfer function's decode: an encoded 0..1 value to linear light. Values past 1 continue the
    /// curve (an HDR value stays above 1); negative values are 0.
    [[nodiscard]] inline float SrgbToLinear(const float encoded)
    {
        if (!(encoded > 0.0f))
        {
            return 0.0f; // also NaN
        }
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    /// The sRGB transfer function's encode: linear light to an encoded value, 0 for negative or NaN input
    [[nodiscard]] inline float LinearToSrgb(const float linear)
    {
        if (!(linear > 0.0f))
        {
            return 0.0f;
        }
        return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    /// SrgbToLinear of an 8-bit value (0..255), from a table
    [[nodiscard]] inline float SrgbByteToLinear(const unsigned char encoded)
    {
        struct Table
        {
            float values[256];
            Table()
            {
                for (int i = 0; i < 256; ++i)
                {
                    values[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
                }
            }
        };
        static const Table table;
        return table.values[encoded];
    }
}
