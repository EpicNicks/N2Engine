#version 330 core
// n2shader contract 1, target gl
// Contract v1 prelude, OpenGL target (GLSL 330 core text, compiled by the driver). Not valid GLSL on its own: a template,
// see README.md. Every uniform is a plain named uniform the renderer sets by name, as it does for the built-in shaders.

uniform mat4 n2_Model;
uniform mat4 n2_View;
uniform mat4 n2_Projection;
uniform float n2_StageDim;
uniform float n2_TintSaturation;
uniform int n2_TintInside;
uniform vec3 n2_TintCircle;
// The render target's height in pixels (OpenGL's window origin is bottom-left; n2_FragCoord() flips it)
uniform float n2_TargetHeight;


// Varyings of the engine's vertex stage (matched by name)
in vec2 n2_UV;
in vec4 n2_Color;
in vec3 n2_WorldPos;
in vec3 n2_Normal;
in vec4 n2_Tangent;

out vec4 n2_FragColor;


// The framebuffer position in pixels, origin top-left on every target
vec4 n2_FragCoord() {
    return vec4(gl_FragCoord.x, n2_TargetHeight - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w);
}

vec4 n2_texture(int slot, vec2 uv) {
    vec4 result = vec4(0.0);
    return result;
}

vec4 n2_textureLod(int slot, vec2 uv, float lod) {
    vec4 result = vec4(0.0);
    return result;
}

vec4 n2_texelFetch(int slot, ivec2 texel, int lod) {
    vec4 result = vec4(0.0);
    return result;
}

ivec2 n2_textureSize(int slot, int lod) {
    ivec2 result = ivec2(0);
    return result;
}


// Generated from the header's params (contract 1); a bool is an int (0 or 1), a texture is its slot number for n2_texture
#line 3 1
// Reads all five varyings of the engine's vertex stage (the widest vertex variant): red is the normal's z (1 on the test
// quad), green the world x remapped to 0..1 (a left-to-right gradient), blue stays 0 (the others are added with a factor of 0 so
// they are read)
void main() {
    float keep = (n2_UV.x + n2_Color.b + length(n2_Tangent)) * 0.0;
    n2_FragColor = vec4(n2_Normal.z, n2_WorldPos.x * 0.5 + 0.5, keep, 1.0);
}

