// Contract v1 prelude for a custom VERTEX stage, Vulkan target (GLSL 450 to SPIR-V). Not valid GLSL on its own: a template, see README.md.
// The Material block (the header's params) follows the prelude, so the author's n2_vertex() reads the parameters the fragment stage does.

layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 n2_Model;
    mat4 n2_View;
    mat4 n2_Projection;
    float n2_StageDim;
    float n2_TintSaturation;
    int n2_TintInside;
    vec3 n2_TintCircle;
};

layout(location = 0) in vec3 n2_in_Position;
layout(location = 1) in vec3 n2_in_Normal;
layout(location = 2) in vec2 n2_in_UV;
layout(location = 3) in vec4 n2_in_Color;
layout(location = 4) in vec4 n2_in_Tangent;

layout(location = 0) out vec2 n2_UV;
layout(location = 1) out vec4 n2_Color;
layout(location = 2) out vec3 n2_WorldPos;
layout(location = 3) out vec3 n2_Normal;
layout(location = 4) out vec4 n2_Tangent;

// OpenGL clip space has z from -w to w, Vulkan's from 0 to w
#define N2_CLIP_Z(c) (((c).z + (c).w) * 0.5)
//@include vertex-common.glsl
