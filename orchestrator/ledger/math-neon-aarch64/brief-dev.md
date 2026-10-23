# Task math-neon-aarch64: math: NEON SIMD tier for aarch64 (Vector2/3/4 single and batch ops)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-math-neon-aarch64.

## Issue / plan
Optional follow-up from the Linux arm64 work (approved by the user 2026-10-13 in the 'do all of them' list). Today math/ compiles its SSE2/SSE4.1/AVX code only for x86 (math/include/math/CpuInfo.hpp N2_MATH_X86, math/src/math/Vector{2,3,4}.cpp, Vector3.hpp function-pointer table via SetSIMDLevel) and aarch64 runs the scalar fallback. Scope: add a NEON implementation for aarch64 mirroring the existing SIMD operation set (the same operations that have SSE2 versions: add/sub/scalar mul/div/dot/cross/length/normalize/distance/min/max/abs/floor/ceil/round and the batch operations) behind a new SIMDLevel (e.g. NEON) that is selected on aarch64 (NEON/ASIMD is baseline on aarch64, so no runtime detection beyond the compile-time architecture check; keep the SIMDLevel enum/API compatible: existing values keep their meaning, SetSIMDLevel/ClampSIMDLevel clamp to what the target supports, requesting an x86 tier on aarch64 clamps down). x86 and Windows behaviour, results and performance must be unchanged (do not touch the x86 code paths other than the minimum needed to share the table/enum). Results must match the scalar reference within the EXISTING test tolerances; do not loosen any tolerance; if a test encodes a bit-exact x86 result that NEON cannot match, stop and report needsUser. Use <arm_neon.h> intrinsics guarded by __aarch64__ (MSVC ARM64 only if trivially safe; otherwise guard so other aarch64 compilers use scalar). Add tests that run on every leg: on aarch64 the NEON level is selected and agrees with scalar on a set of vectors including edge cases (zero vectors, normalization of tiny/zero vectors, negative zero, NaN/inf behaviour as the scalar path defines it, batch counts that are not multiples of the lane width, unaligned data); on x86 the new tests degrade to the existing scalar-vs-SIMD style comparison. Update docs/math.html (SIMD tiers, NEON on aarch64, how SetSIMDLevel clamps). Nothing can be built locally; CI is the build, and the `linux (Release, arm64, Clang)` leg runs on real arm64 hardware and is the verification of NEON; the required leg is `linux (Release, x86_64, Clang)`. Make sure the arm64 job's math tests actually exercise the NEON level (assert the level in a test).

## Allowed paths (regex, repo-relative)
- ^math/
- ^tests/math/
- ^tests/CMakeLists\.txt$
- ^docs/math\.html$
- ^docs/testing\.html$

## Acceptance
- All Tests-workflow jobs green incl. linux arm64
- NEON level is selected and tested on arm64; results agree with scalar within existing tolerances
- x86/Windows math code paths unchanged in behaviour
- docs/math.html describes the NEON tier
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check the x86 paths are untouched, intrinsics are used correctly (lane counts, tails, unaligned loads), no tolerance loosened, the arm64 job really runs the NEON level. Unverified: performance on real hardware (no benchmark in CI).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs math-neon-aarch64` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-math-neon-aarch64` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/math-neon-aarch64/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-math-neon-aarch64 --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/math-neon-aarch64/dev.json: {"pr":<number>,"branch":"feat-math-neon-aarch64","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-math-neon-aarch64` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
