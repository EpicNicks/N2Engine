# Task phys-selection-lows: Physics backend selection follow-ups (J2/J3 lows): the Deterministic preset refuses to start until the backend implements layers/triggers/events/queries, a Jolt-only build selects Jolt by default, ChooseBackend checks PhysX is built, correct wording when the Jolt request comes from the host option, physics.html Not-verified callout
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-phys-selection-lows.

## Issue / plan
Follow-up of the merged Jolt J2 (#199) and J3 (#205) PRs; the user accepted these LOW findings as a follow-up. Design C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md sections 4.2 and 6.3. Verify each against master first. (1) engine/src/network/NetworkValidation.cpp (~line 87): the Deterministic preset currently STARTS on Jolt (strict-FP build, physics.deterministic on) although Jolt's triggers, layers, contact/trigger events and queries are not implemented yet (J4/J5): add to IPhysicsBackend a declared set of implemented feature groups (layers/matrix, triggers, contact+trigger events, queries; PhysX declares all; JoltBackend declares only what exists on master) and make the Deterministic preset validation REFUSE to start (clear error naming the missing groups and the backend) unless every group is implemented; tests for refusal and acceptance (use a fake backend declaring all groups for the acceptance path); (2) engine/src/Application.cpp (~line 394) and engine/include/engine/physics/PhysicsBackendKind.hpp ChooseBackend (~line 383): in a build with N2ENGINE_USE_JOLT=ON and N2ENGINE_USE_PHYSX=OFF a project without physics.backend gets NO physics because the default/fallback is always PhysX; and ChooseBackend falls back to PhysX without checking PhysX is built (the warning says 'using physx instead' when nothing will start). New rule: AUTO/unset selects PhysX when built, else Jolt when built, else none; any fallback names only a backend that is compiled in; if the requested backend is unavailable and no other is built, a clear refusal message; unit tests for every build-flag combination the selection function can see (make it testable without the real flags, e.g. pass availability in); (3) engine/src/Application.cpp (~line 160): when the Jolt request comes from the host's ApplicationOptions (no project setting), the fallback/refusal message and the Physics health row still say 'physics.backend is jolt': name the actual source (host option vs project setting); (4) docs/physics.html (~line 444): the Jolt 'Not verified' callout says cross-leg hash equality is unverified, but JoltBackendDeterminismTest asserts the golden constants on every leg and docs/testing.html tabulates equal hashes: correct it to what is and is not verified. Update docs/physics.html, docs/netcode.html (Deterministic preset requirements) and docs/application.html where the selection text changes. Record the new default rule for Jolt-only builds as a deviation for the user. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved (other Jolt items run in parallel: resolve conflicts keeping both sides); your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Deterministic preset refuses while the backend lacks any feature group; tested
- Selection: PhysX if built else Jolt else none; fallbacks only to built backends; source-aware messages; tested
- physics.html callout corrected
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: every build-flag combination, the refusal cannot be bypassed by the host option, messages name the right source, docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs phys-selection-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-phys-selection-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/phys-selection-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-phys-selection-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/phys-selection-lows/dev.json: {"pr":<number>,"branch":"fix-phys-selection-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-phys-selection-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
