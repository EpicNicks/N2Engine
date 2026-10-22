# Task math-avx-vector2-batch-fix: Fix Vector2::DotBatch/LengthBatch (AVX): wrong results for 2 of every 4 elements (#131)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-vector2-avx-batch. Closes ##131.

## Issue / plan
GitHub issue #131 (read it via the gh wrapper first: api repos/EpicNicks/N2Engine/issues/131). Found by the NEON review: Vector2::DotBatch and LengthBatch (the AVX path, math/src/math/Vector2.cpp) read lanes 0,2,4,6 of a hadd result that holds its values in lanes 0,1,4,5, so for batches of 4 or more elements results 2 and 4 of each group of four are wrong on every x86 build that defines __AVX__ (all required CI legs). Scope: (1) fix the lane extraction/permute so every result is correct, for all counts including tails that are not multiples of the group size; (2) in tests/math/math_types/SimdDispatchTests.cpp remove the `#ifndef __AVX__` exclusion (marked with issue #131) so the Dot/Length batch comparisons run on x86 legs against the scalar reference with the EXISTING tolerances; add targeted tests with distinct values per element (so a permuted lane cannot pass by accident) for counts 1..17 and unaligned starts as the existing tests do; (3) remove the known-flaw note about this bug from docs/math.html (it links #131) and say in the PR body that it closes #131; (4) check the other batch functions in Vector2.cpp (and Vector3/4 AVX batch code) for the same hadd-lane pattern and report any further instance as a finding in your answer (fix it only if it is the identical bug and has the same test approach; otherwise leave it and list it). Do not change the NEON path or any API. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`; the arm64 leg exercises NEON, not this path; the Windows legs build with /arch:AVX).

## Allowed paths (regex, repo-relative)
- ^math/
- ^tests/math/
- ^docs/math\.html$
- ^docs/testing\.html$

## Acceptance
- All Tests-workflow jobs green
- Dot/Length batch results correct for every element on AVX builds, with the exclusion removed and tests for counts 1..17
- docs/math.html no longer lists the flaw
- No API change; NEON path untouched
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check the permute/extract logic by hand for all 8 lanes, the tail handling, the tests use per-element-distinct data, no tolerance loosened, and that other batch ops with the same pattern were examined.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs math-avx-vector2-batch-fix` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-vector2-avx-batch` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/math-avx-vector2-batch-fix/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-vector2-avx-batch --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/math-avx-vector2-batch-fix/dev.json: {"pr":<number>,"branch":"fix-vector2-avx-batch","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-vector2-avx-batch` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
