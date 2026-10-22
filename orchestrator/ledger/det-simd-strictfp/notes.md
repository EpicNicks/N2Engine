Accepted by the user 2026-10-13:
- Unsupported forced tier is lowered with a warning (existing ClampSIMDLevel), not refused; documented as a design decision
- Application::Init now loads project.n2proj once before InitializeSIMD and reuses it for physics.gpu (same behaviour, moved earlier)
- Bit-difference table re-traced per tier from each SetSIMDLevel: adds Vector3 ScalarDiv at SSE2/SSE4.1/AVX, Vector3 Dot zero-sign at SSE4.1/AVX, Matrix4 Multiply and TransformPoint; Vector3 Length/Distance at SSE4.1 moved to not-a-difference (dpps documented order equals scalar for squares)
- Init no longer reports a bad math.simdTier; ApplyProjectSettings in the hosts warns once (same as physics.gpu)
- STRICT_FP also rejects -Ofast, -mtune=native, /fp:fast, /fp:contract in CMAKE_*_FLAGS at configure time; clang-cl also gets -ffp-contract=off
- Forced-Scalar bit-exact tests compare against reference expressions written in the tests (scalar impls are private), covering Vector3 (now incl. ScalarDiv, Distance), Quaternion, Matrix4 multiply
