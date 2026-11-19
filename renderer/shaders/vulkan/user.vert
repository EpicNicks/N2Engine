#version 450

// The vertex stage of every user shader (proposal section 3.3: the engine writes the vertex main, so a user shader cannot
// forget the clip-space z remap). It writes the contract's varyings (renderer/shaders/user/prelude/vulkan.glsl declares the
// matching fragment inputs, locations 0-4) and reads the Common::Vertex attributes they need.
//
// COMPILED 32 TIMES, once per set of varyings the fragment stage reads (renderer/CMakeLists.txt defines N2_OUT_UV,
// N2_OUT_COLOR, N2_OUT_WORLDPOS, N2_OUT_NORMAL and N2_OUT_TANGENT per variant). The validation layer warns about a vertex
// output no fragment input reads and about an attribute no vertex shader reads, and the tests fail on a warning, so each
// pipeline pairs a fragment stage with the variant that writes exactly what it reads (and its vertex input provides exactly
// the attributes that variant reads: UserVertexAttributes in UserShader.hpp). The PerDraw block is the built-ins'
// (GpuShaderInterface.hpp kPerDraw) under the contract's n2_ names.
layout(location = 0) in vec3 aPos;
#ifdef N2_OUT_NORMAL
layout(location = 1) in vec3 aNormal;
#endif
#ifdef N2_OUT_UV
layout(location = 2) in vec2 aTexCoord;
#endif
#ifdef N2_OUT_COLOR
layout(location = 3) in vec4 aColor;
#endif
#ifdef N2_OUT_TANGENT
layout(location = 4) in vec4 aTangent;
#endif

layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 n2_Model;
    mat4 n2_View;
    mat4 n2_Projection;
    float n2_StageDim;
    float n2_TintSaturation;
    int n2_TintInside;
    vec3 n2_TintCircle;
};

#ifdef N2_OUT_UV
layout(location = 0) out vec2 n2_UV;
#endif
#ifdef N2_OUT_COLOR
layout(location = 1) out vec4 n2_Color;
#endif
#ifdef N2_OUT_WORLDPOS
layout(location = 2) out vec3 n2_WorldPos;
#endif
#ifdef N2_OUT_NORMAL
layout(location = 3) out vec3 n2_Normal;
#endif
#ifdef N2_OUT_TANGENT
layout(location = 4) out vec4 n2_Tangent;
#endif

void main() {
    vec4 worldPos = n2_Model * vec4(aPos, 1.0);
#ifdef N2_OUT_WORLDPOS
    n2_WorldPos = worldPos.xyz;
#endif

#ifdef N2_OUT_NORMAL
    // The lit shader's normal maths (lit.vert): the normal matrix
    mat3 normalMatrix = transpose(inverse(mat3(n2_Model)));
    n2_Normal = normalize(normalMatrix * aNormal);
#endif
#ifdef N2_OUT_TANGENT
    // A unit tangent, the bitangent sign flipped by a mirroring model matrix (lit.vert)
    vec3 worldTangent = mat3(n2_Model) * aTangent.xyz;
    float worldTangentLength = length(worldTangent);
    worldTangent = worldTangentLength > 0.0 ? worldTangent / worldTangentLength : vec3(0.0);
    n2_Tangent = vec4(worldTangent, aTangent.w * (determinant(mat3(n2_Model)) < 0.0 ? -1.0 : 1.0));
#endif
#ifdef N2_OUT_UV
    n2_UV = aTexCoord;
#endif
#ifdef N2_OUT_COLOR
    n2_Color = aColor;
#endif
    gl_Position = n2_Projection * n2_View * worldPos;
    // OpenGL clip space has z from -w to w, Vulkan's from 0 to w
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
