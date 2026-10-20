// The WebGPU port of renderer/shaders/vulkan/text.{vert,frag} (the port of OpenGLRenderer.cpp's textVert and textFrag): the
// antialiased SDF text shader. uTexture is a single-channel SDF atlas (r8unorm: it samples as (r, 0, 0, 1), and the shader
// reads .r): the glyph edge is at 0.5 (128 in the atlas bytes), inside is higher. The coverage ramps from 0 to 1 across about
// one screen pixel around the edge, measured with fwidth. Effects (outline, shadow, softness) are material values that are
// all 0 when off. The conventions (resource layout, transposed matrices, clip space) are those of unlit.wgsl.

struct PerDraw {
    uModel: mat4x4<f32>,
    uView: mat4x4<f32>,
    uProjection: mat4x4<f32>,
}

// Effects (TextDrawing::EffectUniforms), all 0 when off: distances are in atlas value units. The members take 92 bytes;
// WGSL rounds the struct up to its alignment (96), and the dynamic-offset range covers 128 either way.
struct Material {
    uAlbedo: vec4<f32>,
    uOutline: f32,              // the outline's outer edge is at 0.5 - uOutline
    uOutlineColor: vec4<f32>,
    uSoftness: f32,             // the edge ramp's half width, at least the antialiasing ramp's
    uShadowColor: vec4<f32>,    // alpha 0: no shadow
    uShadowOffset: vec2<f32>,   // in atlas uv: the shadow at uv is the shape at uv - uShadowOffset
    uShadowSoftness: f32,
}

@group(0) @binding(0) var<uniform> perDraw: PerDraw;

@group(1) @binding(0) var<uniform> material: Material;
@group(1) @binding(1) var uTexture: texture_2d<f32>;
@group(1) @binding(6) var uTextureSampler: sampler;

struct VertexInput {
    @location(0) aPos: vec3<f32>,
    @location(2) aTexCoord: vec2<f32>,
    @location(3) aColor: vec4<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) fragTexCoord: vec2<f32>,
    @location(1) fragColor: vec4<f32>,
}

struct FragmentInput {
    @location(0) fragTexCoord: vec2<f32>,
    @location(1) fragColor: vec4<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    var clip = perDraw.uProjection * perDraw.uView * perDraw.uModel * vec4<f32>(input.aPos, 1.0);
    // OpenGL clip space has z from -w to w, WebGPU's from 0 to w
    clip.z = (clip.z + clip.w) * 0.5;
    output.position = clip;
    output.fragTexCoord = input.aTexCoord;
    output.fragColor = input.aColor;
    return output;
}

// a over b, neither premultiplied
fn over(a: vec4<f32>, b: vec4<f32>) -> vec4<f32> {
    let alpha = a.a + b.a * (1.0 - a.a);
    if (alpha <= 0.0) {
        return vec4<f32>(0.0);
    }
    return vec4<f32>((a.rgb * a.a + b.rgb * b.a * (1.0 - a.a)) / alpha, alpha);
}

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4<f32> {
    // Every sample and derivative is in uniform control flow: at the top, or under a condition on a uniform-buffer value
    let sdf = textureSample(uTexture, uTextureSampler, input.fragTexCoord).r;
    // Half the change over one pixel each side: a ramp one pixel wide (or wider, with softness)
    let edgeWidth = max(max(0.5 * fwidth(sdf), 1e-4), material.uSoftness);
    let coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf);

    let color = material.uAlbedo * input.fragColor;
    let alpha = color.a * coverage;

    // Without effects this is the whole shader: the face colour, alpha-scaled by coverage
    var result = vec4<f32>(color.rgb, alpha);
    if (material.uOutline > 0.0) {
        // The outline fills from its outer edge inwards, under the face
        let outlineEdge = 0.5 - material.uOutline;
        // The ramp's lower end never goes below 0, the distance at the quad's border: minified text
        // has a wide antialiasing ramp, which would otherwise give the whole quad some coverage
        let outlineCoverage = smoothstep(max(outlineEdge - edgeWidth, 0.0), outlineEdge + edgeWidth, sdf);
        result = over(result, vec4<f32>(material.uOutlineColor.rgb, material.uOutlineColor.a * outlineCoverage));
    }
    if (material.uShadowColor.a > 0.0) {
        // The shadow is the face-plus-outline shape, moved, under both
        let shadowSdf = textureSample(uTexture, uTextureSampler, input.fragTexCoord - material.uShadowOffset).r;
        let shadowWidth = max(max(0.5 * fwidth(shadowSdf), 1e-4), material.uShadowSoftness);
        let shadowEdge = 0.5 - material.uOutline;
        let shadowCoverage = smoothstep(max(shadowEdge - shadowWidth, 0.0), shadowEdge + shadowWidth, shadowSdf);
        result = over(result, vec4<f32>(material.uShadowColor.rgb, material.uShadowColor.a * shadowCoverage));
    }

    if (result.a < 0.01) {
        discard;
    }
    return result;
}
