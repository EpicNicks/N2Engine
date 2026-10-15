#version 450

// The Vulkan port of OpenGLRenderer.cpp's unlitVert (GLSL 330). Compiled to SPIR-V at build time (renderer/CMakeLists.txt)
// and embedded; read as text by tests/renderer/vulkan/VulkanShaderParityTests.cpp, which ties the names, block offsets
// and attribute locations below to VulkanShaderInterface.hpp and to Material::ApplyUniforms.
//
// Only the attributes the shader reads are declared (the pipeline provides exactly those): the layer warns about a
// vertex attribute nothing consumes.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;

// The per-draw block (set 0, a dynamic offset per draw). The engine's matrices are row-major and GL uploads them
// transposed (GL_TRUE); row_major here reads the same rows, so they are copied as they are.
layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
};

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
    // OpenGL clip space has z from -w to w, Vulkan's from 0 to w
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    fragTexCoord = aTexCoord;
    fragColor = aColor;
}
