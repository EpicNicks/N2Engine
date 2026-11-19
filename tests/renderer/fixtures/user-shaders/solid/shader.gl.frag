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
uniform vec4 u_Color;
uniform float u_Gain;
uniform vec3 u_Offset;
uniform int u_Flag;
uniform int u_Steps;
uniform vec2 u_Pair;
#line 12 1
// A parameter block that exercises std140 packing (vec3 then bool in one 16 bytes, int, vec2): the draw's colour is
// u_Color * u_Gain + u_Offset, inverted when u_Flag is set. u_Steps and u_Pair are read so the compiler keeps them.
void main() {
    vec3 rgb = u_Color.rgb * u_Gain + u_Offset;
    rgb += float(u_Steps - 2) * 0.0 + (u_Pair.x - 0.25) * 0.0 + (u_Pair.y - 0.5) * 0.0;
    if (u_Flag != 0) {
        rgb = vec3(1.0) - rgb;
    }
    n2_FragColor = vec4(rgb, u_Color.a);
}

