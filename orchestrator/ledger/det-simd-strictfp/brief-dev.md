# Task det-simd-strictfp: Determinism S1 (B1, B2): force-scalar SIMD tier setting and a strict-FP compile profile (issue #157)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-determinism-simd-strictfp.

## Issue / plan
Analysis: C:/Method Notes Obsidian/orchestrator/proposals/physx-limitations-and-determinism.md (read sections 3, 4.4 and 5 fully first: the blocker inventory B1-B10 with file/line evidence; the line numbers are from master 7222391, re-verify each against the code before editing; docs/prose must be verified against CODE). This is stage 'S1 (cheap)' of the determinism plan (issue #157): small, CI-verifiable changes that are useful even without netcode (reproducible replays and tests). HARD RULES: default behaviour of every existing project/scene/test must be UNCHANGED unless the new setting/option is switched on; no protocol change; no network presets yet (that is stage S2, not this item); no new third-party dependency. Nothing builds locally; the PR's CI (9 checks) is the test, so write tests that run on every leg and keep platform-specific expectations out of them. List unverified items honestly. No Co-Authored-By trailer and no 'Generated with Claude Code' footer. SCOPE: (B1) add a way to choose the math SIMD tier instead of always auto-detecting: a project/engine setting (follow how existing engine settings are read in Application.cpp near the Math::InitializeSIMD call; values Auto (default) / Scalar / the other tiers that exist) applied BEFORE any scene load, with a log line naming the tier in use; forcing a tier the CPU lacks must refuse or clamp with a warning (decide, document). Tests: with Scalar forced, a fixed set of Vector3/Quaternion/Matrix operations gives results equal to the scalar reference functions EXACTLY (ASSERT bit equality), and Auto still passes the existing tolerance tests. Document which ops differ bit-wise across tiers today (Quaternion dot ordering, Vector3 normalize reciprocal-multiply vs divide per B1) in docs: do NOT change the tier implementations in this item. (B2) add CMake option N2ENGINE_STRICT_FP (default OFF) that on GCC/Clang adds -ffp-contract=off to ALL engine targets (not only math/aarch64), on MSVC sets /fp:precise explicitly and never /fp:contract, forbids -march=native/-ffast-math, and defines N2_STRICT_FP; expose a runtime query (e.g. Math::IsStrictFp()) for the later startup check. Turn the option ON in ONE existing CI leg that is cheap (pick the linux x86_64 GCC 14 leg unless a better one exists; do not rename any job) so the flag is exercised. Docs: a short section in the relevant docs page (find the math/testing docs).

## Allowed paths (regex, repo-relative)
- ^math/
- ^engine/
- ^CMakeLists\.txt$
- ^cmake/
- ^tests/
- ^docs/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Default build and all existing tests unchanged
- Forcing Scalar gives bit-exact results vs the scalar reference in a test
- N2ENGINE_STRICT_FP exists, is ON in one CI leg, defines N2_STRICT_FP, and is documented
- Tier in use is logged at startup
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: confirm defaults unchanged, the setting is applied before scene load, flags are applied to all targets only when the option is ON, MSVC and GCC/Clang branches both correct, the CI leg edit changes no job name.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs det-simd-strictfp` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-determinism-simd-strictfp` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/det-simd-strictfp/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-determinism-simd-strictfp --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/det-simd-strictfp/dev.json: {"pr":<number>,"branch":"feat-determinism-simd-strictfp","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-determinism-simd-strictfp` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
