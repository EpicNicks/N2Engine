// The WebGPU port of renderer/shaders/vulkan/lit.{vert,frag} (the port of OpenGLRenderer.cpp's litVert and litFrag): the same
// maths and constants (the PBR part is the software renderer's LightingMath.hpp line for line). The conventions (resource
// layout, transposed matrices, clip space, i32 for a GLSL int) are those of unlit.wgsl; what is particular to this one:
//  - inverse(): WGSL has no inverse, so inverse3 below is the 3x3 adjugate over the determinant (what GLSL's inverse(mat3)
//    computes, including the sign: a mirroring model matrix flips the normal exactly as in GLSL).
//  - Sampling: WGSL's uniformity analysis rejects a derivative-taking textureSample in non-uniform control flow, and a loop
//    left by a non-uniform continue is non-uniform afterwards. So all five textures are sampled up front, each under a
//    condition on a uniform-buffer value (the material flags), before the first discard and before the lighting loops; the
//    results are used where the GLSL sampled. A flag that is 0 skips the sample, as in the GLSL.
//  - Block layout: the Lighting block's light structs are WGSL structs whose members sit at the table's offsets and whose
//    sizes round up to the array strides (32, 48, 64).

struct PerDraw {
    uModel: mat4x4<f32>,
    uView: mat4x4<f32>,
    uProjection: mat4x4<f32>,
}

// Material block. Emissive: added after lighting, unaffected by it. Occlusion: scales the ambient light by the texture's red
// channel, mixed in by uOcclusionStrength (0 = none, 1 = full). Normal map (tangent space, green up): uNormalScale multiplies
// its x and y; a vertex without a tangent is drawn with its own normal. uPbr = 1 is metallic-roughness PBR instead of
// Blinn-Phong: uMetallic and 1 - uSmoothness (the roughness) times the texture's blue and green channels.
struct Material {
    uAlbedo: vec4<f32>,
    uHasTexture: i32,
    uMetallic: f32,
    uSmoothness: f32,
    uAlphaCutoff: f32,
    uEmissive: vec3<f32>,
    uHasEmissiveTexture: i32,
    uHasOcclusionTexture: i32,
    uOcclusionStrength: f32,
    uHasNormalTexture: i32,
    uNormalScale: f32,
    uPbr: i32,
    uHasMetallicRoughnessTexture: i32,
}

struct DirectionalLight {
    direction: vec3<f32>,
    color: vec3<f32>,
    intensity: f32,
}

struct PointLight {
    position: vec3<f32>,
    color: vec3<f32>,
    intensity: f32,
    range: f32,
    attenuation: f32,
}

struct SpotLight {
    position: vec3<f32>,
    direction: vec3<f32>,
    color: vec3<f32>,
    intensity: f32,
    range: f32,
    cosInner: f32,  // cos of the cone half-angles, set by UpdateSceneLighting
    cosOuter: f32,
}

// Lighting block (written by UpdateSceneLighting). Linear lighting: sRGB textures are rgba8unorm-srgb and decode themselves
// when sampled; the colour target is UNORM, so uEncodeOutput (1 when linear lighting is on) makes this shader encode its own
// output.
struct Lighting {
    uCameraPos: vec3<f32>,
    uEncodeOutput: i32,
    uAmbientLight: vec3<f32>,
    uNumDirectionalLights: i32,
    uNumPointLights: i32,
    uNumSpotLights: i32,
    uDirectionalLights: array<DirectionalLight, 2>,
    uPointLights: array<PointLight, 8>,
    uSpotLights: array<SpotLight, 4>,
}

@group(0) @binding(0) var<uniform> perDraw: PerDraw;
@group(0) @binding(1) var<uniform> sceneLighting: Lighting;

@group(1) @binding(0) var<uniform> material: Material;
@group(1) @binding(1) var uTexture: texture_2d<f32>;
@group(1) @binding(2) var uEmissiveTexture: texture_2d<f32>;
@group(1) @binding(3) var uOcclusionTexture: texture_2d<f32>;
@group(1) @binding(4) var uNormalTexture: texture_2d<f32>;
@group(1) @binding(5) var uMetallicRoughnessTexture: texture_2d<f32>;
@group(1) @binding(6) var uTextureSampler: sampler;
@group(1) @binding(7) var uEmissiveTextureSampler: sampler;
@group(1) @binding(8) var uOcclusionTextureSampler: sampler;
@group(1) @binding(9) var uNormalTextureSampler: sampler;
@group(1) @binding(10) var uMetallicRoughnessTextureSampler: sampler;

struct VertexInput {
    @location(0) aPos: vec3<f32>,
    @location(1) aNormal: vec3<f32>,
    @location(2) aTexCoord: vec2<f32>,
    @location(3) aColor: vec4<f32>,
    @location(4) aTangent: vec4<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) fragNormal: vec3<f32>,
    @location(1) fragWorldPos: vec3<f32>,
    @location(2) fragTexCoord: vec2<f32>,
    @location(3) fragColor: vec4<f32>,
    @location(4) fragTangent: vec4<f32>,
}

struct FragmentInput {
    @location(0) fragNormal: vec3<f32>,
    @location(1) fragWorldPos: vec3<f32>,
    @location(2) fragTexCoord: vec2<f32>,
    @location(3) fragColor: vec4<f32>,
    @location(4) fragTangent: vec4<f32>,
}

// The inverse of a 3x3 matrix: the adjugate (the cross products of the columns, as rows) over the determinant
fn inverse3(m: mat3x3<f32>) -> mat3x3<f32> {
    let row0 = cross(m[1], m[2]);
    let row1 = cross(m[2], m[0]);
    let row2 = cross(m[0], m[1]);
    let determinantOfM = dot(m[0], row0);
    return transpose(mat3x3<f32>(row0, row1, row2)) * (1.0 / determinantOfM);
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let worldPos = perDraw.uModel * vec4<f32>(input.aPos, 1.0);
    output.fragWorldPos = worldPos.xyz;

    let model3 = mat3x3<f32>(perDraw.uModel[0].xyz, perDraw.uModel[1].xyz, perDraw.uModel[2].xyz);

    // Transform normal to world space (proper method)
    let normalMatrix = transpose(inverse3(model3));
    output.fragNormal = normalize(normalMatrix * input.aNormal);

    // The tangent is a direction: it goes by the model matrix and is made unit length here, so a tiny model scale
    // can't shrink it below the fragment shader's test (a vertex with none, xyz = 0, stays 0). A mirroring model
    // matrix (negative determinant) flips the bitangent, so the handedness w too.
    var worldTangent = model3 * input.aTangent.xyz;
    let worldTangentLength = length(worldTangent);
    worldTangent = select(vec3<f32>(0.0), worldTangent / worldTangentLength, worldTangentLength > 0.0);
    output.fragTangent = vec4<f32>(worldTangent, input.aTangent.w * select(1.0, -1.0, determinant(model3) < 0.0));

    output.fragTexCoord = input.aTexCoord;
    output.fragColor = input.aColor;
    var clip = perDraw.uProjection * perDraw.uView * worldPos;
    // OpenGL clip space has z from -w to w, WebGPU's from 0 to w
    clip.z = (clip.z + clip.w) * 0.5;
    output.position = clip;
    return output;
}

// ---- PBR (the software renderer's LightingMath.hpp, line for line) ----
const PI: f32 = 3.14159265358979323846;
const MIN_PERCEPTUAL_ROUGHNESS: f32 = 0.045;
const DIELECTRIC_F0: f32 = 0.04;

fn ggxDistribution(nDotH: f32, alpha: f32) -> f32 {
    let a2 = alpha * alpha;
    let d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn smithGgxVisibility(nDotL: f32, nDotV: f32, alpha: f32) -> f32 {
    let a2 = alpha * alpha;
    let gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    let gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 0.00001);
}

fn schlickWeight(vDotH: f32) -> f32 {
    let m = clamp(1.0 - vDotH, 0.0, 1.0);
    let m2 = m * m;
    return m2 * m2 * m;
}

// One light's reply: (diffuse / pi + specular) * pi * NdotL * radiance, where radiance is the light's
// colour x intensity (x attenuation x cone). L points at the light.
fn pbrLight(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, radiance: vec3<f32>, baseColor: vec3<f32>, metallic: f32,
            perceptualRoughness: f32, f0: vec3<f32>) -> vec3<f32> {
    let nDotL = dot(N, L);
    if (nDotL <= 0.0) {
        return vec3<f32>(0.0);
    }
    var H = L + V;
    let h2 = dot(H, H);
    if (h2 < 0.000000000001) {
        return vec3<f32>(0.0);
    }
    H = H * inverseSqrt(h2);
    let nDotV = max(dot(N, V), 0.0001);
    let nDotH = max(dot(N, H), 0.0);
    let vDotH = max(dot(V, H), 0.0);
    let alpha = perceptualRoughness * perceptualRoughness;
    let w = schlickWeight(vDotH);
    let F = f0 + (1.0 - f0) * w;
    let fd = DIELECTRIC_F0 + (1.0 - DIELECTRIC_F0) * w;
    let diffuse = (1.0 - metallic) * (1.0 - fd) * baseColor;
    let specular = F * (PI * ggxDistribution(nDotH, alpha) * smithGgxVisibility(nDotL, nDotV, alpha));
    return (diffuse + specular) * nDotL * radiance;
}

fn calculateAttenuation(dist: f32, lightRange: f32, attenuation: f32) -> f32 {
    let d = dist / lightRange;
    return 1.0 / (1.0 + attenuation * d * d);
}

// The sRGB encode (the software renderer's LinearToSrgb)
fn linearToSrgb(value: vec3<f32>) -> vec3<f32> {
    let c = max(value, vec3<f32>(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, step(vec3<f32>(0.0031308), c));
}

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4<f32> {
    // Every texture is sampled here, in uniform control flow (see the header), and used below where the GLSL sampled it
    var baseSample = vec4<f32>(1.0);
    if (material.uHasTexture != 0) {
        baseSample = textureSample(uTexture, uTextureSampler, input.fragTexCoord);
    }
    var normalSample = vec4<f32>(0.0);
    if (material.uHasNormalTexture != 0) {
        normalSample = textureSample(uNormalTexture, uNormalTextureSampler, input.fragTexCoord);
    }
    var metallicRoughnessSample = vec4<f32>(1.0);
    if (material.uPbr != 0 && material.uHasMetallicRoughnessTexture != 0) {
        metallicRoughnessSample = textureSample(uMetallicRoughnessTexture, uMetallicRoughnessTextureSampler, input.fragTexCoord);
    }
    var occlusionSample = vec4<f32>(1.0);
    if (material.uHasOcclusionTexture != 0) {
        occlusionSample = textureSample(uOcclusionTexture, uOcclusionTextureSampler, input.fragTexCoord);
    }
    var emissiveSample = vec4<f32>(1.0);
    if (material.uHasEmissiveTexture != 0) {
        emissiveSample = textureSample(uEmissiveTexture, uEmissiveTextureSampler, input.fragTexCoord);
    }

    // Base colour: uAlbedo x texture x vertex colour, alpha-tested against uAlphaCutoff
    var albedo = material.uAlbedo * baseSample;
    albedo = albedo * input.fragColor;
    if (albedo.a < material.uAlphaCutoff) {
        discard;
    }

    var N = normalize(input.fragNormal);
    if (material.uHasNormalTexture != 0) {
        var m = normalSample.xyz * 2.0 - 1.0;
        // The tangent made perpendicular to the normal; none (zero) leaves the normal as it is. The vertex
        // stage made it unit length, so the test is relative.
        var T = input.fragTangent.xyz - N * dot(N, input.fragTangent.xyz);
        let tangentLength2 = dot(T, T);
        if (tangentLength2 > 0.00000001) {
            T = T * inverseSqrt(tangentLength2);
            let B = cross(N, T) * select(1.0, -1.0, input.fragTangent.w < 0.0);
            m = vec3<f32>(m.xy * material.uNormalScale, m.z);
            N = normalize(T * m.x + B * m.y + N * m.z);
        }
    }
    let V = normalize(sceneLighting.uCameraPos - input.fragWorldPos);

    // PBR surface: metallic and roughness (with the texture's blue and green), and the reflectance at normal
    // incidence. pbrSum collects the lights; unused with Blinn-Phong.
    var metallic = clamp(material.uMetallic, 0.0, 1.0);
    var roughnessTexture = 1.0;
    if (material.uPbr != 0 && material.uHasMetallicRoughnessTexture != 0) {
        metallic = metallic * metallicRoughnessSample.b;
        roughnessTexture = metallicRoughnessSample.g;
    }
    let perceptualRoughness = clamp((1.0 - material.uSmoothness) * roughnessTexture, MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    let f0 = mix(vec3<f32>(DIELECTRIC_F0), albedo.rgb, metallic);
    var pbrSum = vec3<f32>(0.0);

    // Start with ambient, dimmed by the occlusion texture
    var occlusion = 1.0;
    if (material.uHasOcclusionTexture != 0) {
        occlusion = 1.0 + material.uOcclusionStrength * (occlusionSample.r - 1.0);
    }
    var lighting = sceneLighting.uAmbientLight * occlusion;

    // Directional lights
    for (var i: i32 = 0; i < sceneLighting.uNumDirectionalLights; i++) {
        let light = sceneLighting.uDirectionalLights[i];
        let L = normalize(-light.direction);
        if (material.uPbr != 0) {
            pbrSum = pbrSum + pbrLight(N, V, L, light.color * light.intensity, albedo.rgb, metallic, perceptualRoughness, f0);
            continue;
        }
        let NdotL = max(dot(N, L), 0.0);

        // Diffuse
        let diffuse = light.color * light.intensity * NdotL;

        // Specular (Blinn-Phong)
        let H = normalize(L + V);
        let NdotH = max(dot(N, H), 0.0);
        let shininess = mix(4.0, 256.0, material.uSmoothness);
        let spec = pow(NdotH, shininess);
        let specular = light.color * spec * 0.3;

        lighting = lighting + diffuse + specular;
    }

    // Point lights
    for (var i: i32 = 0; i < sceneLighting.uNumPointLights; i++) {
        let light = sceneLighting.uPointLights[i];
        let lightToFrag = input.fragWorldPos - light.position;
        let dist = length(lightToFrag);

        if (dist > light.range) {
            continue;
        }

        let L = normalize(-lightToFrag);
        let NdotL = max(dot(N, L), 0.0);
        let attenuation = calculateAttenuation(dist, light.range, light.attenuation);

        if (material.uPbr != 0) {
            pbrSum = pbrSum + pbrLight(N, V, L, light.color * light.intensity * attenuation, albedo.rgb, metallic,
                                       perceptualRoughness, f0);
            continue;
        }

        let diffuse = light.color * light.intensity * NdotL * attenuation;

        let H = normalize(L + V);
        let NdotH = max(dot(N, H), 0.0);
        let shininess = mix(4.0, 256.0, material.uSmoothness);
        let spec = pow(NdotH, shininess);
        let specular = light.color * spec * 0.3 * attenuation;

        lighting = lighting + diffuse + specular;
    }

    // Spot lights
    for (var i: i32 = 0; i < sceneLighting.uNumSpotLights; i++) {
        let light = sceneLighting.uSpotLights[i];
        let lightToFrag = input.fragWorldPos - light.position;
        let dist = length(lightToFrag);

        if (dist > light.range) {
            continue;
        }

        // At the light itself there is no direction (and normalize would give NaN): none
        let L = select(vec3<f32>(0.0), -lightToFrag / dist, dist > 0.000001);

        // Cone: 1 inside the inner cone, 0 outside the outer, a ramp in the cosine between (the
        // software renderer's SpotConeFactor). The axis is a unit vector.
        let theta = dot(L, -light.direction);
        let spotIntensity = clamp((theta - light.cosOuter) / max(light.cosInner - light.cosOuter, 0.0001), 0.0, 1.0);

        if (spotIntensity == 0.0) {
            continue;
        }

        let NdotL = max(dot(N, L), 0.0);
        let attenuation = calculateAttenuation(dist, light.range, 1.0);

        if (material.uPbr != 0) {
            pbrSum = pbrSum + pbrLight(N, V, L, light.color * light.intensity * attenuation * spotIntensity, albedo.rgb,
                                       metallic, perceptualRoughness, f0);
            continue;
        }

        let diffuse = light.color * light.intensity * NdotL * attenuation * spotIntensity;

        let H = normalize(L + V);
        let NdotH = max(dot(N, H), 0.0);
        let shininess = mix(4.0, 256.0, material.uSmoothness);
        let spec = pow(NdotH, shininess);
        let specular = light.color * spec * 0.3 * attenuation * spotIntensity;

        lighting = lighting + diffuse + specular;
    }

    var emissive = material.uEmissive;
    if (material.uHasEmissiveTexture != 0) {
        emissive = emissive * emissiveSample.rgb;
    }

    var color = lighting * albedo.rgb + emissive;
    if (material.uPbr != 0) {
        // The ambient light as a uniform environment: its diffuse reply and its specular reply (the analytic
        // fit of the environment BRDF), both scaled by the occlusion; then the lights and the emissive
        let nDotV = max(dot(N, V), 0.0001);
        let r = perceptualRoughness * vec4<f32>(-1.0, -0.0275, -0.572, 0.022) + vec4<f32>(1.0, 0.0425, 1.04, -0.04);
        let a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
        let envAB = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
        let ambient = sceneLighting.uAmbientLight * occlusion;
        let specularReflectance = f0 * envAB.x + envAB.y;
        let ambientDiffuse = ambient * (1.0 - metallic) * albedo.rgb * (1.0 - specularReflectance);
        let ambientSpecular = ambient * specularReflectance;
        color = pbrSum + ambientDiffuse + ambientSpecular + emissive;
    }
    if (sceneLighting.uEncodeOutput != 0) {
        color = linearToSrgb(color);
    }
    return vec4<f32>(color, albedo.a);
}
