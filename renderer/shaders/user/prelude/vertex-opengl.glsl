// Contract v1 prelude for a custom VERTEX stage, OpenGL target (GLSL 330 core text, compiled by the driver). Not valid GLSL on its own.
// Plain named uniforms the renderer sets by name, as for the fragment stage; the varyings match the fragment stage's by name.

uniform mat4 n2_Model;
uniform mat4 n2_View;
uniform mat4 n2_Projection;
uniform float n2_StageDim;
uniform float n2_TintSaturation;
uniform int n2_TintInside;
uniform vec3 n2_TintCircle;

layout(location = 0) in vec3 n2_in_Position;
layout(location = 1) in vec3 n2_in_Normal;
layout(location = 2) in vec2 n2_in_UV;
layout(location = 3) in vec4 n2_in_Color;
layout(location = 4) in vec4 n2_in_Tangent;

out vec2 n2_UV;
out vec4 n2_Color;
out vec3 n2_WorldPos;
out vec3 n2_Normal;
out vec4 n2_Tangent;

// OpenGL's own clip space: no remap
#define N2_CLIP_Z(c) ((c).z)
//@include vertex-common.glsl
