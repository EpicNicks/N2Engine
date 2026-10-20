// The WebGPU port of renderer/shaders/vulkan/unlit.{vert,frag} (itself the port of OpenGLRenderer.cpp's unlitVert and
// unlitFrag): uAlbedo x texture x vertex colour, alpha-tested against uAlphaCutoff (0 keeps every fragment). One module
// with both entry points (vs_main, fs_main), hand-written (design W1 of #155), tied to renderer/common/GpuShaderInterface.hpp
// by tests/renderer/webgpu/WebGpuShaderParityTests.cpp (which reads this file as text) and validated by Tint and naga in CI
// (scripts/validate-wgsl.sh).
//
// Conventions shared by the three WebGPU shaders (docs/application.html#webgpu-shaders):
//  - Resource layout: group 0 is the frame (binding 0 the per-draw block), group 1 the material (binding 0 the material block,
//    then the textures at the table's bindings 1 to 5 and their samplers at the same number plus 5, 6 to 10: WGSL has no
//    combined image sampler, so every Vulkan sampler2D is a texture_2d plus a sampler, named uXxx and uXxxSampler).
//  - Matrices: WGSL has only column-major matrices and no row_major block qualifier. The engine's matrices are row-major, so
//    the backend TRANSPOSES each of them when it fills the per-draw block (the bytes are then the matrix's columns, which is
//    what mat4x4<f32> reads), and the maths below reads exactly like the GLSL (uProjection * uView * uModel * v).
//  - Clip space: the engine's projection gives OpenGL clip space (z from -w to w); WebGPU's z is 0 to w like Vulkan's, so
//    each vertex shader remaps z. WebGPU's y is up with row 0 of the framebuffer at the top, which is what Vulkan gets from its
//    flipped viewport, so no y flip is needed here.
//  - Layout: every uniform block member has the name the interface table gives it, at the same offset (the table's std140
//    offsets are valid WGSL uniform offsets for these blocks), and a GLSL int is an i32.

struct PerDraw {
    uModel: mat4x4<f32>,
    uView: mat4x4<f32>,
    uProjection: mat4x4<f32>,
}

struct Material {
    uAlbedo: vec4<f32>,
    uHasTexture: i32,
    uAlphaCutoff: f32,
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

@fragment
fn fs_main(input: FragmentInput) -> @location(0) vec4<f32> {
    var color = material.uAlbedo;
    // The condition is a uniform-buffer value, so the sample is in uniform control flow
    if (material.uHasTexture != 0) {
        color = color * textureSample(uTexture, uTextureSampler, input.fragTexCoord);
    }
    color = color * input.fragColor;
    if (color.a < material.uAlphaCutoff) {
        discard;
    }
    return color;
}
