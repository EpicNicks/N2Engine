# Task jolt-j7-determinism-ci: Jolt J7: strict-FP legs with Jolt, the physics scene corpus runner, cross-leg determinism aggregation job with goldens (non-required)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-jolt-j7-determinism-ci.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (P1-P11 ACCEPTED), section 5 (5.1 scene corpus of 10 scenes, 5.2 legs and flag matrix, 5.3 golden hashes and aggregation), row J7 (line ~248), question Q1/Q2/Q11. J1-J6 are merged: read the Jolt backend, SaveState/RestoreState/HashState, the existing determinism tests (JoltBackendDeterminismTest and its golden), the PhysX determinism option tests from #161, .github/workflows/tests.yml (every leg and how N2ENGINE_STRICT_FP is set), CMake strict-FP handling, and the existing netcode C2 physics-free canary if present. SCOPE: (1) a headless corpus runner (a test executable or gtest in the existing engine test binary that can also write JSON: pick what fits) that runs the 10 scenes of 5.1 on Jolt (600 ticks at 60 Hz, hash every 60 ticks; HashState; event log hashes; query result hashes; snapshot round trip; rollback simulation) and writes determinism-hashes.json (scene, tick, hash, build fingerprint: compiler, arch, flags) ; the same-leg negative control that also runs PhysX where built and prints its hashes; (2) CI: turn N2ENGINE_STRICT_FP=ON with Jolt on the legs of 5.2 (windows Release, linux x86_64 clang, linux arm64 clang, linux x86_64 GCC 14 (already strict), web/node) without breaking the existing tests/budgets (measure nothing locally: be conservative; if strict FP on a leg is known to break other tests, put the corpus on a dedicated new job/leg instead of changing the existing one and say so); each leg uploads determinism-hashes.json as an artifact; (3) a new 'determinism' job with needs: all those legs that downloads the artifacts and compares them with each other AND with the committed tests/golden/physics-determinism.json, printing the first differing scene/tick per leg; the job is NON-required (do not add it to required checks; document how to make it required later per P9) ; a script (tools/ or scripts/) regenerates the goldens from the Linux leg's artifact and a docs paragraph says goldens change only in a PR that says why; since the first goldens cannot be produced locally, the first run may have no golden: implement 'golden missing => compare legs only and print the hashes in the job summary, and the PR report says the user/orchestrator must commit them from the first green run'; (4) docs: physics.html determinism section and testing.html rows, true against the code and workflow; the docs-only check job must still work. IMPORTANT: a mismatch between legs is a REAL RESULT, not a failure to hide: if a leg differs from the others, the job reports it clearly; do not tune goldens or loosen compares to make it green; record in the PR report what the first CI run showed. Nothing builds locally; re-read every YAML/CMake/C++ edit twice. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/
- ^tools/
- ^scripts/

## Acceptance
- Corpus runner writes determinism-hashes.json with the 10 scenes on Jolt
- Strict FP + Jolt on the legs of the matrix (or dedicated legs) with artifacts
- determinism aggregation job compares legs and goldens, non-required; goldens regeneration script
- Docs true; existing tests and budgets unaffected; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (numerics/CI): workflow YAML valid and needs: graph right, artifact names consistent, comparison logic correct (cannot pass vacuously when an artifact is missing), no leg silently skipped, strict-FP flags actually reach the Jolt and engine targets on each compiler, corpus scenes deterministic by construction (no unordered iteration, one RNG draw per statement), compile-level correctness.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs jolt-j7-determinism-ci` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-jolt-j7-determinism-ci` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/jolt-j7-determinism-ci/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-jolt-j7-determinism-ci --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/jolt-j7-determinism-ci/dev.json: {"pr":<number>,"branch":"feat-jolt-j7-determinism-ci","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-jolt-j7-determinism-ci` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
