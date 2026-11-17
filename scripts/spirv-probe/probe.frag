#version 450
// Stage 0 of the user shaders (orchestrator/proposals/user-shaders.md, section 10): a one-file SPIR-V module for
// scripts/validate-wgsl.sh to hand to `tint --format wgsl`, proving that the Dawn `tint` of the pinned release has a SPIR-V
// reader on this platform (the user-shader cook translates SPIR-V to WGSL with it). Separate texture and sampler, a uniform
// block and a loop: the same shapes the generated user shaders have, nothing more.
layout(set = 1, binding = 0, std140) uniform Material {
    vec4 u_Tint;
    int u_Steps;
};
layout(set = 1, binding = 1) uniform texture2D probeTexture;
layout(set = 1, binding = 6) uniform sampler probeSampler;
layout(location = 0) in vec2 probeUV;
layout(location = 0) out vec4 probeColor;

void main() {
    vec4 color = u_Tint * texture(sampler2D(probeTexture, probeSampler), probeUV);
    for (int i = 0; i < u_Steps; i++) {
        color.rgb *= 0.5;
    }
    probeColor = color;
}
