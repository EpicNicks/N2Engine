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
