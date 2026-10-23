#version 450

// The Vulkan port of OpenGLRenderer.cpp's litFrag (GLSL 330): the same maths and constants (the PBR part is the software
// renderer's LightingMath.hpp line for line). Compiled and pipeline-checked in stage B of #43; drawn since stage C (the
// lighting block is filled by UpdateSceneLighting, see GpuLighting.hpp).
//
// Every uniform is a member of one of two blocks (GLSL 450 for Vulkan has no loose uniforms), declared without an
// instance name so the shader body is the GL one. A bool becomes an int (a bool is not allowed in a block): 0 or 1.
//
// Set 0 is the frame: binding 0 the per-draw block (the vertex shader's) and binding 1 the lighting block below. Set 1
// is the material: binding 0 the material block, bindings 1 to 5 the five samplers, in AuxTexture order after the base
// texture.

// Material block (one dynamic-offset range per draw)
layout(set = 1, binding = 0, std140) uniform Material {
    vec4 uAlbedo;
    int uHasTexture;
    float uMetallic;
    float uSmoothness;
    float uAlphaCutoff;
    // Emissive: added after lighting, unaffected by it. Occlusion: scales the ambient light by the texture's
    // red channel, mixed in by uOcclusionStrength (0 = none, 1 = full).
    vec3 uEmissive;
    int uHasEmissiveTexture;
    int uHasOcclusionTexture;
    float uOcclusionStrength;
    // Normal map (tangent space, green up): uNormalScale multiplies its x and y. A vertex without a tangent is
    // drawn with its own normal.
    int uHasNormalTexture;
    float uNormalScale;
    // Metallic-roughness PBR instead of Blinn-Phong (uPbr = 1): uMetallic and 1 - uSmoothness (the roughness)
    // times the texture's blue and green channels. The ambient light is a uniform environment (no image based lighting).
    int uPbr;
    int uHasMetallicRoughnessTexture;
};

layout(set = 1, binding = 1) uniform sampler2D uTexture;
layout(set = 1, binding = 2) uniform sampler2D uEmissiveTexture;
layout(set = 1, binding = 3) uniform sampler2D uOcclusionTexture;
layout(set = 1, binding = 4) uniform sampler2D uNormalTexture;
layout(set = 1, binding = 5) uniform sampler2D uMetallicRoughnessTexture;

// Scene lighting
struct DirectionalLight {
    vec3 direction;
    vec3 color;
    float intensity;
};

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
    float attenuation;
};

struct SpotLight {
    vec3 position;
    vec3 direction;
    vec3 color;
    float intensity;
    float range;
    float cosInner;  // cos of the cone half-angles, set by UpdateSceneLighting
    float cosOuter;
};

// Lighting block (set 0, binding 1; written once per frame by UpdateSceneLighting). Linear lighting: sRGB textures are
// SRGB8_ALPHA8 and decode themselves when sampled; the colour target is UNORM, so uEncodeOutput (1 when linear lighting
// is on) makes this shader encode its own output.
layout(set = 0, binding = 1, std140) uniform Lighting {
    vec3 uCameraPos;
    int uEncodeOutput;
    vec3 uAmbientLight;
    int uNumDirectionalLights;
    int uNumPointLights;
    int uNumSpotLights;
    DirectionalLight uDirectionalLights[2];
    PointLight uPointLights[8];
    SpotLight uSpotLights[4];
};

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragTangent;

layout(location = 0) out vec4 FragColor;

// ---- PBR (the software renderer's LightingMath.hpp, line for line) ----
const float PI = 3.14159265358979323846;
const float MIN_PERCEPTUAL_ROUGHNESS = 0.045;
const float DIELECTRIC_F0 = 0.04;

float ggxDistribution(float nDotH, float alpha) {
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float smithGgxVisibility(float nDotL, float nDotV, float alpha) {
    float a2 = alpha * alpha;
    float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 0.00001);
}

float schlickWeight(float vDotH) {
    float m = clamp(1.0 - vDotH, 0.0, 1.0);
    float m2 = m * m;
    return m2 * m2 * m;
}

// One light's reply: (diffuse / pi + specular) * pi * NdotL * radiance, where radiance is the light's
// colour x intensity (x attenuation x cone). L points at the light.
vec3 pbrLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 baseColor, float metallic, float perceptualRoughness, vec3 f0) {
    float nDotL = dot(N, L);
    if (nDotL <= 0.0) {
        return vec3(0.0);
    }
    vec3 H = L + V;
    float h2 = dot(H, H);
    if (h2 < 0.000000000001) {
        return vec3(0.0);
    }
    H *= inversesqrt(h2);
    float nDotV = max(dot(N, V), 0.0001);
    float nDotH = max(dot(N, H), 0.0);
    float vDotH = max(dot(V, H), 0.0);
    float alpha = perceptualRoughness * perceptualRoughness;
    float w = schlickWeight(vDotH);
    vec3 F = f0 + (1.0 - f0) * w;
    float fd = DIELECTRIC_F0 + (1.0 - DIELECTRIC_F0) * w;
    vec3 diffuse = (1.0 - metallic) * (1.0 - fd) * baseColor;
    vec3 specular = F * (PI * ggxDistribution(nDotH, alpha) * smithGgxVisibility(nDotL, nDotV, alpha));
    return (diffuse + specular) * nDotL * radiance;
}

float calculateAttenuation(float distance, float range, float attenuation) {
    float d = distance / range;
    return 1.0 / (1.0 + attenuation * d * d);
}

// The sRGB encode (the software renderer's LinearToSrgb)
vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    // Base colour: uAlbedo x texture x vertex colour, alpha-tested against uAlphaCutoff
    vec4 albedo = uAlbedo;
    if (uHasTexture != 0) {
        albedo *= texture(uTexture, fragTexCoord);
    }
    albedo *= fragColor;
    if (albedo.a < uAlphaCutoff) {
        discard;
    }

    vec3 N = normalize(fragNormal);
    if (uHasNormalTexture != 0) {
        // The fetch is outside the branch below (a texture lookup in non-uniform control flow has undefined
        // derivatives, so undefined mip selection)
        vec3 m = texture(uNormalTexture, fragTexCoord).xyz * 2.0 - 1.0;
        // The tangent made perpendicular to the normal; none (zero) leaves the normal as it is. The vertex
        // stage made it unit length, so the test is relative.
        vec3 T = fragTangent.xyz - N * dot(N, fragTangent.xyz);
        float tangentLength2 = dot(T, T);
        if (tangentLength2 > 0.00000001) {
            T *= inversesqrt(tangentLength2);
            vec3 B = cross(N, T) * (fragTangent.w < 0.0 ? -1.0 : 1.0);
            m.xy *= uNormalScale;
            N = normalize(T * m.x + B * m.y + N * m.z);
        }
    }
    vec3 V = normalize(uCameraPos - fragWorldPos);

    // PBR surface: metallic and roughness (with the texture's blue and green), and the reflectance at normal
    // incidence. pbrSum collects the lights; unused with Blinn-Phong.
    float metallic = clamp(uMetallic, 0.0, 1.0);
    float roughnessTexture = 1.0;
    if (uPbr != 0 && uHasMetallicRoughnessTexture != 0) {
        vec4 mr = texture(uMetallicRoughnessTexture, fragTexCoord);
        metallic *= mr.b;
        roughnessTexture = mr.g;
    }
    float perceptualRoughness = clamp((1.0 - uSmoothness) * roughnessTexture, MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    vec3 f0 = mix(vec3(DIELECTRIC_F0), albedo.rgb, metallic);
    vec3 pbrSum = vec3(0.0);

    // Start with ambient, dimmed by the occlusion texture
    float occlusion = 1.0;
    if (uHasOcclusionTexture != 0) {
        occlusion = 1.0 + uOcclusionStrength * (texture(uOcclusionTexture, fragTexCoord).r - 1.0);
    }
    vec3 lighting = uAmbientLight * occlusion;

    // Directional lights
    for (int i = 0; i < uNumDirectionalLights; i++) {
        vec3 L = normalize(-uDirectionalLights[i].direction);
        if (uPbr != 0) {
            pbrSum += pbrLight(N, V, L, uDirectionalLights[i].color * uDirectionalLights[i].intensity,
                               albedo.rgb, metallic, perceptualRoughness, f0);
            continue;
        }
        float NdotL = max(dot(N, L), 0.0);

        // Diffuse
        vec3 diffuse = uDirectionalLights[i].color *
                      uDirectionalLights[i].intensity *
                      NdotL;

        // Specular (Blinn-Phong)
        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float shininess = mix(4.0, 256.0, uSmoothness);
        float spec = pow(NdotH, shininess);
        vec3 specular = uDirectionalLights[i].color * spec * 0.3;

        lighting += diffuse + specular;
    }

    // Point lights
    for (int i = 0; i < uNumPointLights; i++) {
        vec3 lightToFrag = fragWorldPos - uPointLights[i].position;
        float distance = length(lightToFrag);

        if (distance > uPointLights[i].range)
            continue;

        vec3 L = normalize(-lightToFrag);
        float NdotL = max(dot(N, L), 0.0);
        float attenuation = calculateAttenuation(
            distance,
            uPointLights[i].range,
            uPointLights[i].attenuation
        );

        if (uPbr != 0) {
            pbrSum += pbrLight(N, V, L, uPointLights[i].color * uPointLights[i].intensity * attenuation,
                               albedo.rgb, metallic, perceptualRoughness, f0);
            continue;
        }

        vec3 diffuse = uPointLights[i].color *
                      uPointLights[i].intensity *
                      NdotL *
                      attenuation;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float shininess = mix(4.0, 256.0, uSmoothness);
        float spec = pow(NdotH, shininess);
        vec3 specular = uPointLights[i].color * spec * 0.3 * attenuation;

        lighting += diffuse + specular;
    }

    // Spot lights
    for (int i = 0; i < uNumSpotLights; i++) {
        vec3 lightToFrag = fragWorldPos - uSpotLights[i].position;
        float distance = length(lightToFrag);

        if (distance > uSpotLights[i].range)
            continue;

        // At the light itself there is no direction (and normalize would give NaN): none
        vec3 L = distance > 0.000001 ? -lightToFrag / distance : vec3(0.0);

        // Cone: 1 inside the inner cone, 0 outside the outer, a ramp in the cosine between (the
        // software renderer's SpotConeFactor). The axis is a unit vector.
        float theta = dot(L, -uSpotLights[i].direction);
        float spotIntensity = clamp(
            (theta - uSpotLights[i].cosOuter) / max(uSpotLights[i].cosInner - uSpotLights[i].cosOuter, 0.0001),
            0.0,
            1.0
        );

        if (spotIntensity == 0.0)
            continue;

        float NdotL = max(dot(N, L), 0.0);
        float attenuation = calculateAttenuation(distance, uSpotLights[i].range, 1.0);

        if (uPbr != 0) {
            pbrSum += pbrLight(N, V, L,
                               uSpotLights[i].color * uSpotLights[i].intensity * attenuation * spotIntensity,
                               albedo.rgb, metallic, perceptualRoughness, f0);
            continue;
        }

        vec3 diffuse = uSpotLights[i].color *
                      uSpotLights[i].intensity *
                      NdotL *
                      attenuation *
                      spotIntensity;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float shininess = mix(4.0, 256.0, uSmoothness);
        float spec = pow(NdotH, shininess);
        vec3 specular = uSpotLights[i].color * spec * 0.3 * attenuation * spotIntensity;

        lighting += diffuse + specular;
    }

    vec3 emissive = uEmissive;
    if (uHasEmissiveTexture != 0) {
        emissive *= texture(uEmissiveTexture, fragTexCoord).rgb;
    }

    vec3 color = lighting * albedo.rgb + emissive;
    if (uPbr != 0) {
        // The ambient light as a uniform environment: its diffuse reply and its specular reply (the analytic
        // fit of the environment BRDF), both scaled by the occlusion; then the lights and the emissive
        float nDotV = max(dot(N, V), 0.0001);
        vec4 r = perceptualRoughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
        float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
        vec2 envAB = vec2(-1.04, 1.04) * a004 + r.zw;
        vec3 ambient = uAmbientLight * occlusion;
        vec3 specularReflectance = f0 * envAB.x + envAB.y;
        vec3 ambientDiffuse = ambient * (1.0 - metallic) * albedo.rgb * (1.0 - specularReflectance);
        vec3 ambientSpecular = ambient * specularReflectance;
        color = pbrSum + ambientDiffuse + ambientSpecular + emissive;
    }
    if (uEncodeOutput != 0) {
        color = linearToSrgb(color);
    }
    FragColor = vec4(color, albedo.a);
}
