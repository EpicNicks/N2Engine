// The vertex stage of every WebGPU user shader (proposal section 3.3: the engine writes the vertex main, so a user shader cannot
// forget the clip-space z remap). The WebGPU twin of renderer/shaders/vulkan/user.vert: it writes the contract's five varyings
// (locations 0-4: n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent) and the user's fragment module (the WGSL Tint wrote from the
// cooked SPIR-V, entry point main) reads the ones it declares. Unlike Vulkan's validation layer, WebGPU accepts a vertex output no
// fragment input reads and an attribute nothing reads, so ONE variant serves every shader.
//
// Conventions as unlit.wgsl: group 0 binding 0 is the per-draw block (matrices transposed on upload, so they are declared
// column-major here and in the user's fragment module: transposing a second time would be the "transpose twice" trap), the clip z is
// remapped from GL's -w..w to 0..w, and no y flip is needed. This module has only this vertex entry point, vs_main.

struct PerDraw {
    uModel: mat4x4<f32>,
    uView: mat4x4<f32>,
    uProjection: mat4x4<f32>,
    uStageDim: f32,
    uTintSaturation: f32,
    uTintInside: i32,
    uTintCircle: vec3<f32>,
}

@group(0) @binding(0) var<uniform> perDraw: PerDraw;

struct VertexInput {
    @location(0) aPos: vec3<f32>,
    @location(1) aNormal: vec3<f32>,
    @location(2) aTexCoord: vec2<f32>,
    @location(3) aColor: vec4<f32>,
    @location(4) aTangent: vec4<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) n2_UV: vec2<f32>,
    @location(1) n2_Color: vec4<f32>,
    @location(2) n2_WorldPos: vec3<f32>,
    @location(3) n2_Normal: vec3<f32>,
    @location(4) n2_Tangent: vec4<f32>,
}

// What GLSL's inverse(mat3) computes (the 3x3 adjugate over the determinant), as lit.wgsl: a mirroring model matrix flips the
// normal exactly as in the Vulkan user.vert
fn inverse3(m: mat3x3<f32>) -> mat3x3<f32> {
    let a = m[0];
    let b = m[1];
    let c = m[2];
    let r0 = cross(b, c);
    let r1 = cross(c, a);
    let r2 = cross(a, b);
    let det = dot(a, r0);
    return transpose(mat3x3<f32>(r0, r1, r2)) * (1.0 / det);
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let model = perDraw.uModel;
    let worldPos = model * vec4<f32>(input.aPos, 1.0);
    let model3 = mat3x3<f32>(model[0].xyz, model[1].xyz, model[2].xyz);

    // The lit shader's normal maths (lit.vert): the normal matrix
    output.n2_Normal = normalize(transpose(inverse3(model3)) * input.aNormal);
    // A unit tangent, the bitangent sign flipped by a mirroring model matrix (lit.vert)
    var worldTangent = model3 * input.aTangent.xyz;
    let worldTangentLength = length(worldTangent);
    worldTangent = select(vec3<f32>(0.0), worldTangent / worldTangentLength, worldTangentLength > 0.0);
    output.n2_Tangent = vec4<f32>(worldTangent, input.aTangent.w * select(1.0, -1.0, determinant(model3) < 0.0));
    output.n2_WorldPos = worldPos.xyz;
    output.n2_UV = input.aTexCoord;
    output.n2_Color = input.aColor;

    var clip = perDraw.uProjection * perDraw.uView * worldPos;
    // OpenGL clip space has z from -w to w, WebGPU's from 0 to w
    clip.z = (clip.z + clip.w) * 0.5;
    output.position = clip;
    return output;
}
