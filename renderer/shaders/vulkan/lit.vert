#version 450

// The Vulkan port of OpenGLRenderer.cpp's litVert (GLSL 330). Compiled and pipeline-checked in stage B of #43; lit
// drawing is turned on in stage C.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;

layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
};

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragColor;
layout(location = 4) out vec4 fragTangent;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    fragWorldPos = worldPos.xyz;

    // Transform normal to world space (proper method)
    mat3 normalMatrix = transpose(inverse(mat3(uModel)));
    fragNormal = normalize(normalMatrix * aNormal);

    // The tangent is a direction: it goes by the model matrix and is made unit length here, so a tiny model scale
    // can't shrink it below the fragment shader's test (a vertex with none, xyz = 0, stays 0). A mirroring model
    // matrix (negative determinant) flips the bitangent, so the handedness w too.
    vec3 worldTangent = mat3(uModel) * aTangent.xyz;
    float worldTangentLength = length(worldTangent);
    worldTangent = worldTangentLength > 0.0 ? worldTangent / worldTangentLength : vec3(0.0);
    fragTangent = vec4(worldTangent, aTangent.w * (determinant(mat3(uModel)) < 0.0 ? -1.0 : 1.0));

    fragTexCoord = aTexCoord;
    fragColor = aColor;
    gl_Position = uProjection * uView * worldPos;
    // OpenGL clip space has z from -w to w, Vulkan's from 0 to w
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
