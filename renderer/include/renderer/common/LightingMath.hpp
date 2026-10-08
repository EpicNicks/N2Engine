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

    // ===== Metallic-roughness PBR (the lit shaders' optional shading model, uPbr) =====
    //
    // Cook-Torrance with a GGX distribution, height-correlated Smith visibility and Schlick's Fresnel, as in the
    // glTF 2.0 specification's reference BRDF. A light's colour x intensity is its radiance divided by pi, the
    // convention that gives a white light of intensity 1 on a white dielectric (NdotL = 1) a diffuse reply of about
    // 1, as the Blinn-Phong model gives: so a light's reply is  (diffuse / pi + specular) * pi * NdotL * light.

    inline constexpr float kPi = 3.14159265358979323846f;
    /// The smallest perceptual roughness: below it the GGX highlight of a punctual light shrinks to a point
    inline constexpr float kMinPerceptualRoughness = 0.045f;
    /// The reflectance at normal incidence of a dielectric (glTF's fixed 4%)
    inline constexpr float kDielectricF0 = 0.04f;

    /// The perceptual roughness of a material: (1 - smoothness) times the metallic-roughness texture's green
    /// channel (1 with no texture), clamped to kMinPerceptualRoughness..1
    [[nodiscard]] inline float PerceptualRoughness(const float smoothness, const float textureGreen = 1.0f)
    {
        return std::clamp((1.0f - smoothness) * textureGreen, kMinPerceptualRoughness, 1.0f);
    }

    /// GGX (Trowbridge-Reitz) normal distribution. alpha = perceptual roughness squared.
    [[nodiscard]] inline float GgxDistribution(const float nDotH, const float alpha)
    {
        const float a2 = alpha * alpha;
        const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
        return a2 / (kPi * d * d);
    }

    /// Height-correlated Smith-GGX visibility, G / (4 NdotL NdotV)
    [[nodiscard]] inline float SmithGgxVisibility(const float nDotL, const float nDotV, const float alpha)
    {
        const float a2 = alpha * alpha;
        const float gv = nDotL * std::sqrt(nDotV * nDotV * (1.0f - a2) + a2);
        const float gl = nDotV * std::sqrt(nDotL * nDotL * (1.0f - a2) + a2);
        return 0.5f / std::max(gv + gl, 0.00001f);
    }

    /// Schlick's Fresnel weight (1 - VdotH)^5: the reflectance is F0 + (1 - F0) * weight
    [[nodiscard]] inline float SchlickWeight(const float vDotH)
    {
        const float m = std::clamp(1.0f - vDotH, 0.0f, 1.0f);
        const float m2 = m * m;
        return m2 * m2 * m;
    }

    /// The scale and bias of the specular reflectance F0 * scale + bias of a uniform environment (Karis's analytic
    /// fit of the split-sum environment BRDF), for the ambient light: the PBR model has no image based lighting
    struct EnvBrdf
    {
        float scale;
        float bias;
    };

    [[nodiscard]] inline EnvBrdf EnvironmentBrdf(const float perceptualRoughness, const float nDotV)
    {
        const float rx = perceptualRoughness * -1.0f + 1.0f;
        const float ry = perceptualRoughness * -0.0275f + 0.0425f;
        const float rz = perceptualRoughness * -0.572f + 1.04f;
        const float rw = perceptualRoughness * 0.022f + -0.04f;
        const float a004 = std::min(rx * rx, std::exp2(-9.28f * nDotV)) * rx + ry;
        return EnvBrdf{-1.04f * a004 + rz, 1.04f * a004 + rw};
    }
}
