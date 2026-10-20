// Must be REJECTED by Tint (scripts/validate-wgsl.sh checks it; naga does not reject it, so it is not asked to): a
// derivative-taking sample in non-uniform control flow, the mistake the lit shader avoids by sampling every texture first.
@group(0) @binding(0) var t: texture_2d<f32>;
@group(0) @binding(1) var s: sampler;

@fragment
fn fs_main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
    if (p.x > 10.0) {
        return textureSample(t, s, p.xy);
    }
    return vec4<f32>(0.0);
}
