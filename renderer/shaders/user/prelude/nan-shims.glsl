// Contract v1 NaN and infinity checks (stage 9b). Added after the target's prelude only for a shader whose stage names isnan or isinf
// (the cook and the C++ twin both look for the word), so every other shader's generated text is exactly what it was.
// WGSL cannot represent NaN or infinity, so Tint and naga reject the built-ins; these test the IEEE bit pattern instead, the same on
// every target. isnan and isinf below are macros for them, so the author writes the built-in's name and gets scalars and vectors.
// A GPU compiler may assume a value is finite and fold the test away; the bits are what the shader computed, which is as much as a
// shader can know.
bool n2_isnan(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u; }
bvec2 n2_isnan(vec2 x) { return bvec2(n2_isnan(x.x), n2_isnan(x.y)); }
bvec3 n2_isnan(vec3 x) { return bvec3(n2_isnan(x.x), n2_isnan(x.y), n2_isnan(x.z)); }
bvec4 n2_isnan(vec4 x) { return bvec4(n2_isnan(x.x), n2_isnan(x.y), n2_isnan(x.z), n2_isnan(x.w)); }
bool n2_isinf(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) == 0x7F800000u; }
bvec2 n2_isinf(vec2 x) { return bvec2(n2_isinf(x.x), n2_isinf(x.y)); }
bvec3 n2_isinf(vec3 x) { return bvec3(n2_isinf(x.x), n2_isinf(x.y), n2_isinf(x.z)); }
bvec4 n2_isinf(vec4 x) { return bvec4(n2_isinf(x.x), n2_isinf(x.y), n2_isinf(x.z), n2_isinf(x.w)); }
#define isnan(x) n2_isnan(x)
#define isinf(x) n2_isinf(x)
