// Contract v1 prelude, Vulkan target (GLSL 450 to SPIR-V). Not valid GLSL on its own: a template, see README.md.

// The per-draw block (set 0, binding 0, 224-byte range). row_major: the engine's matrices are row-major and copied as they are
layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 n2_Model;
    mat4 n2_View;
    mat4 n2_Projection;
    float n2_StageDim;
    float n2_TintSaturation;
    int n2_TintInside;
    vec3 n2_TintCircle;
};

//@if-lighting
//@include lighting-std140.glsl
//@end-if

// Varyings of the engine's vertex stage
layout(location = 0) in vec2 n2_UV;
layout(location = 1) in vec4 n2_Color;
layout(location = 2) in vec3 n2_WorldPos;
layout(location = 3) in vec3 n2_Normal;
layout(location = 4) in vec4 n2_Tangent;

layout(location = 0) out vec4 n2_FragColor;

//@each-texture
layout(set = 1, binding = @TEX_BINDING@) uniform sampler2D n2_tex@SLOT@;
//@end-each

// The framebuffer position in pixels, origin top-left on every target (Vulkan's flipped viewport already counts rows down)
vec4 n2_FragCoord() {
    return gl_FragCoord;
}

vec4 n2_texture(int slot, vec2 uv) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = texture(n2_tex@SLOT@, uv); }
//@end-each
    return result;
}

vec4 n2_textureLod(int slot, vec2 uv, float lod) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = textureLod(n2_tex@SLOT@, uv, lod); }
//@end-each
    return result;
}

vec4 n2_texelFetch(int slot, ivec2 texel, int lod) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = texelFetch(n2_tex@SLOT@, texel, lod); }
//@end-each
    return result;
}

ivec2 n2_textureSize(int slot, int lod) {
    ivec2 result = ivec2(0);
//@each-texture
    if (slot == @SLOT@) { result = textureSize(n2_tex@SLOT@, lod); }
//@end-each
    return result;
}

//@if-lighting
//@include common-lighting.glsl
//@end-if
