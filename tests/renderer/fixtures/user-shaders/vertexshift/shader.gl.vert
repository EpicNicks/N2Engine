#version 330 core
// n2shader contract 1, target gl, vertex stage
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
// The engine's vertex maths, shared by the three targets' custom vertex stages (stage 9d). Included by vertex-vulkan.glsl,
// vertex-webgpu.glsl and vertex-opengl.glsl after they declare the attributes (n2_in_*), the varyings (n2_UV ... n2_Tangent as outputs),
// the PerDraw uniforms and N2_CLIP_Z.

bool n2_positionSet = false;

// Writes the clip-space position, with the target's depth remap applied (the author gives OpenGL clip space, z from -w to w)
void n2_SetPosition(vec4 clip) {
    clip.z = N2_CLIP_Z(clip);
    gl_Position = clip;
    n2_positionSet = true;
}

// What the built-in vertex stage does: world position, normal matrix, unit tangent with the mirror sign, uv and colour. The generated
// main calls it first, then the author's n2_vertex(), which may change any of the five varyings
void n2_DefaultVertex() {
    vec4 worldPos = n2_Model * vec4(n2_in_Position, 1.0);
    n2_WorldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(n2_Model)));
    n2_Normal = normalize(normalMatrix * n2_in_Normal);
    vec3 worldTangent = mat3(n2_Model) * n2_in_Tangent.xyz;
    float worldTangentLength = length(worldTangent);
    worldTangent = worldTangentLength > 0.0 ? worldTangent / worldTangentLength : vec3(0.0);
    n2_Tangent = vec4(worldTangent, n2_in_Tangent.w * (determinant(mat3(n2_Model)) < 0.0 ? -1.0 : 1.0));
    n2_UV = n2_in_UV;
    n2_Color = n2_in_Color;
}


// Generated from the header's params (contract 1); a bool is an int (0 or 1), a texture is its slot number for n2_texture
uniform vec4 u_Color;
uniform float u_Shift;
#line 13 2
// Stage 9d: a custom vertex stage. The engine's default maths ran first; this moves the world position along x by a parameter of the
// Material block (the clip position follows from n2_WorldPos).
void n2_vertex() {
    n2_WorldPos.x += u_Shift;
}

void main() {
    n2_DefaultVertex();
    n2_vertex();
    if (!n2_positionSet) {
        n2_SetPosition(n2_Projection * n2_View * vec4(n2_WorldPos, 1.0));
    }
}
