#version 450

// The Vulkan port of OpenGLRenderer.cpp's textVert (GLSL 330). Same as unlit.vert.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;

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
