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

uniform sampler2D n2_tex0;
uniform sampler2D n2_tex1;

// The framebuffer position in pixels, origin top-left on every target
vec4 n2_FragCoord() {
    return vec4(gl_FragCoord.x, n2_TargetHeight - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w);
}

vec4 n2_texture(int slot, vec2 uv) {
    vec4 result = vec4(0.0);
    if (slot == 0) { result = texture(n2_tex0, uv); }
    if (slot == 1) { result = texture(n2_tex1, uv); }
    return result;
}

vec4 n2_textureLod(int slot, vec2 uv, float lod) {
    vec4 result = vec4(0.0);
    if (slot == 0) { result = textureLod(n2_tex0, uv, lod); }
    if (slot == 1) { result = textureLod(n2_tex1, uv, lod); }
    return result;
}

vec4 n2_texelFetch(int slot, ivec2 texel, int lod) {
    vec4 result = vec4(0.0);
    if (slot == 0) { result = texelFetch(n2_tex0, texel, lod); }
    if (slot == 1) { result = texelFetch(n2_tex1, texel, lod); }
    return result;
}

ivec2 n2_textureSize(int slot, int lod) {
    ivec2 result = ivec2(0);
    if (slot == 0) { result = textureSize(n2_tex0, lod); }
    if (slot == 1) { result = textureSize(n2_tex1, lod); }
    return result;
}


// Cube and 3D textures through 2D atlases (stage 9e). Added after the target's prelude only for a shader that declares a `cube` or a
// `texture3d` parameter, so every other shader's generated text is what it was. Both kinds use one of the five 2D texture slots, so no
// backend, descriptor layout or pipeline changes; what changes is the convention of the image and the lookup below.
//
//   cube      a horizontal strip of six square faces, +X -X +Y -Y +Z -Z (the order of OpenGL and Vulkan cube maps), each face stored as a
//             cube map image is (GL's s and t, t=0 on the first row): the image is (6 * size) x size
//   texture3d `depth` square slices side by side, slice 0 first: the image is (depth * width) x height
//
// Both read with textureLod at level 0 (no derivatives across the atlas's seams, no non-uniform control flow trouble on WebGPU) and
// keep each tap half a texel inside its face or slice, so bilinear filtering never bleeds into the neighbour. A cube face's own edges are
// not blended with the next face (no seamless filtering); a 3D lookup blends the two nearest slices (trilinear). Texture import: filter
// Bilinear or Point, wrap Clamp, mipmaps off (they would only be read at level 0).

vec4 n2_textureCube(int slot, vec3 dir) {
    vec3 a = abs(dir);
    float ma = max(max(a.x, a.y), a.z);
    if (ma <= 0.0) {
        return vec4(0.0);
    }
    float face;
    vec2 sc_tc;
    if (a.x >= a.y && a.x >= a.z) {
        face = dir.x > 0.0 ? 0.0 : 1.0;
        sc_tc = dir.x > 0.0 ? vec2(-dir.z, -dir.y) : vec2(dir.z, -dir.y);
    } else if (a.y >= a.z) {
        face = dir.y > 0.0 ? 2.0 : 3.0;
        sc_tc = dir.y > 0.0 ? vec2(dir.x, dir.z) : vec2(dir.x, -dir.z);
    } else {
        face = dir.z > 0.0 ? 4.0 : 5.0;
        sc_tc = dir.z > 0.0 ? vec2(dir.x, -dir.y) : vec2(-dir.x, -dir.y);
    }
    vec2 st = sc_tc / ma * 0.5 + 0.5;
    float size = max(float(n2_textureSize(slot, 0).y), 1.0);
    float inset = 0.5 / size;
    st = clamp(st, vec2(inset), vec2(1.0 - inset));
    return n2_textureLod(slot, vec2((face + st.x) / 6.0, st.y), 0.0);
}

vec4 n2_texture3D(int slot, int depth, vec3 uvw) {
    float d = float(max(depth, 1));
    float w = clamp(uvw.z, 0.0, 1.0) * d - 0.5;
    float i0 = clamp(floor(w), 0.0, d - 1.0);
    float i1 = min(i0 + 1.0, d - 1.0);
    float f = clamp(w - i0, 0.0, 1.0);
    vec2 size = max(vec2(n2_textureSize(slot, 0)), vec2(1.0));
    vec2 inset = vec2(0.5 / max(size.x / d, 1.0), 0.5 / size.y);
    vec2 xy = clamp(uvw.xy, inset, vec2(1.0) - inset);
    vec4 a = n2_textureLod(slot, vec2((i0 + xy.x) / d, xy.y), 0.0);
    vec4 b = n2_textureLod(slot, vec2((i1 + xy.x) / d, xy.y), 0.0);
    return mix(a, b, f);
}
// Generated from the header's params (contract 1); a bool is an int (0 or 1), a texture is its slot number for n2_texture
uniform vec3 u_Dir;
uniform float u_W;
uniform int u_Mode;
const int u_Sky = 0;
const int u_Vol = 1;
const int u_Vol_depth = 2;
#line 11 1
// Stage 9e: u_Mode 0 reads the cube atlas in the direction u_Dir, 1 reads the volume atlas (two slices) at depth u_W
void main() {
    vec4 color;
    if (u_Mode == 0) {
        color = n2_textureCube(u_Sky, normalize(u_Dir));
    } else {
        color = n2_texture3D(u_Vol, u_Vol_depth, vec3(0.5, 0.5, u_W));
    }
    n2_FragColor = vec4(color.rgb, 1.0);
}

