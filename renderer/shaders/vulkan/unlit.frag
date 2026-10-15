#version 450

// The Vulkan port of OpenGLRenderer.cpp's unlitFrag (GLSL 330): uAlbedo x texture x vertex colour, alpha-tested against
// uAlphaCutoff (0 keeps every fragment). The material block (set 1, binding 0) is one dynamic-offset range per draw;
// the base texture is set 1, binding 1.
layout(set = 1, binding = 0, std140) uniform Material {
    vec4 uAlbedo;
    int uHasTexture;
    float uAlphaCutoff;
};

layout(set = 1, binding = 1) uniform sampler2D uTexture;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 FragColor;

void main() {
    vec4 color = uAlbedo;
    if (uHasTexture != 0) {
        color *= texture(uTexture, fragTexCoord);
    }
    color *= fragColor;
    if (color.a < uAlphaCutoff) {
        discard;
    }
    FragColor = color;
}
