// The post-processing passes and render-texture captures of the WebGPU backend (issue #445; WebGpuPostProcess.cpp): the maths
// of Common::ApplyPostProcessCpu, renderer/shaders/vulkan/post.frag and the OpenGL renderer's inline GLSL. One full-screen
// triangle from the vertex index (no vertex buffer). WebGPU puts NDC y +1 at a target's first row and a texture's first row
// at v = 0, so v = (1 - y) / 2 reads the source the right way up; flipY (a capture) reads it upside down, so the render
// texture's first row is the bottom of the frame.
//
// params.a = (mode, threshold, intensity, invert), params.b = (stepX, stepY, chromatic aberration, vignette),
// params.c = (tint rgb, flipY). Modes: 0 copy (alpha 1), 1 bright, 2 blur, 3 composite, 4 mix (the blur's last pass). Every read is textureSampleLevel at
// level 0, so no read needs derivatives or uniform control flow.

struct PostParams {
    a: vec4<f32>,
    b: vec4<f32>,
    c: vec4<f32>,
};

@group(0) @binding(0) var<uniform> params: PostParams;
@group(0) @binding(1) var sourceTexture: texture_2d<f32>;
@group(0) @binding(2) var bloomTexture: texture_2d<f32>;
@group(0) @binding(3) var linearSampler: sampler;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let pos = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u)) * 2.0 - 1.0;
    var output: VertexOutput;
    output.position = vec4<f32>(pos, 0.0, 1.0);
    var uv = vec2<f32>(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    if (params.c.w > 0.5) {
        uv.y = 1.0 - uv.y;
    }
    output.uv = uv;
    return output;
}

fn readSource(uv: vec2<f32>) -> vec4<f32> {
    return textureSampleLevel(sourceTexture, linearSampler, uv, 0.0);
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let mode = i32(params.a.x + 0.5);
    let uv = input.uv;
    if (mode == 0) {
        return vec4<f32>(readSource(uv).rgb, 1.0);
    }
    if (mode == 1) {
        let c = readSource(uv).rgb;
        let brightest = max(c.r, max(c.g, c.b));
        let weight = max(brightest - params.a.y, 0.0) / max(brightest, 1e-4);
        return vec4<f32>(c * weight, 1.0);
    }
    if (mode == 2) {
        let o1 = params.b.xy;
        let o2 = params.b.xy * 2.0;
        let o3 = params.b.xy * 3.0;
        let o4 = params.b.xy * 4.0;
        var sum = readSource(uv).rgb * 0.2270270270;
        sum = sum + (readSource(uv + o1).rgb + readSource(uv - o1).rgb) * 0.1945945946;
        sum = sum + (readSource(uv + o2).rgb + readSource(uv - o2).rgb) * 0.1216216216;
        sum = sum + (readSource(uv + o3).rgb + readSource(uv - o3).rgb) * 0.0540540541;
        sum = sum + (readSource(uv + o4).rgb + readSource(uv - o4).rgb) * 0.0162162162;
        return vec4<f32>(sum, 1.0);
    }
    if (mode == 4) {
        let sharp = readSource(uv);
        let soft = textureSampleLevel(bloomTexture, linearSampler, uv, 0.0).rgb;
        return vec4<f32>(clamp(mix(sharp.rgb, soft, params.a.z), vec3<f32>(0.0), vec3<f32>(1.0)), sharp.a);
    }
    let d = (uv - vec2<f32>(0.5, 0.5)) * params.b.z;
    let base = readSource(uv);
    var rgb = vec3<f32>(readSource(uv + d).r, base.g, readSource(uv - d).b);
    rgb = rgb + textureSampleLevel(bloomTexture, linearSampler, uv, 0.0).rgb * params.a.z * params.c.rgb;
    rgb = clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    rgb = mix(rgb, vec3<f32>(1.0) - rgb, params.a.w);
    let fromCentre = length(uv - vec2<f32>(0.5, 0.5)) * 1.41421356;
    rgb = clamp(rgb * (1.0 - params.b.w * smoothstep(0.3, 1.0, fromCentre)), vec3<f32>(0.0), vec3<f32>(1.0));
    return vec4<f32>(rgb, base.a);
}
