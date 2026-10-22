Accepted by the user 2026-10-13:
- Added -ffp-contract=off (PUBLIC) to the math target on aarch64 non-MSVC so scalar code is not FMA-contracted and matches NEON and x86; this also changes scalar results on aarch64 for every target linking math.
- NEON requested on x86 and an x86 tier requested on aarch64 both clamp to Scalar (the plan only specified the aarch64 direction); added IsSIMDLevelSupported and Vector3/Vector4::GetSIMDLevel (additive API) so tests can assert the level.
- Quaternion and Matrix have no NEON versions (plan scoped Vector2/3/4): at the NEON tier they stay scalar via a small condition change in their SetSIMDLevel.
- Vector2 has no single-operation SIMD or runtime dispatch, so only its batch ops got NEON (compile-time under N2_MATH_NEON, like the AVX path).
- Unaligned data: Vector3/Vector4 are alignas(16) so a misaligned array would be UB; tests use start offsets inside a pool, in-place calls and sentinels, and genuinely 8-byte-offset Vector2 data.
- Found an existing x86 bug: Vector2::DotBatch and LengthBatch (AVX) read lanes 0,2,4,6 of a hadd that puts results in 0,1,4,5, so results 2 and 4 of each group of four are wrong. Not fixed (x86 out of scope); the new Vector2 test compares those two only when __AVX__ is not defined, and docs/math.html lists it as a known flaw.
