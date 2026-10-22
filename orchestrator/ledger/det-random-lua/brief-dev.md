# Task det-random-lua: Determinism S1 (B4, B6): seedable deterministic random and seedable Lua hash seed / math.random (issue #157)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-determinism-random-lua.

## Issue / plan
Analysis: C:/Method Notes Obsidian/orchestrator/proposals/physx-limitations-and-determinism.md (read sections 3, 4.4 and 5 fully first: the blocker inventory B1-B10 with file/line evidence; the line numbers are from master 7222391, re-verify each against the code before editing; docs/prose must be verified against CODE). This is stage 'S1 (cheap)' of the determinism plan (issue #157): small, CI-verifiable changes that are useful even without netcode (reproducible replays and tests). HARD RULES: default behaviour of every existing project/scene/test must be UNCHANGED unless the new setting/option is switched on; no protocol change; no network presets yet (that is stage S2, not this item); no new third-party dependency. Nothing builds locally; the PR's CI (9 checks) is the test, so write tests that run on every leg and keep platform-specific expectations out of them. List unverified items honestly. No Co-Authored-By trailer and no 'Generated with Claude Code' footer. SCOPE: (B6) add Math::DeterministicRandom (reuse the ParticleRandom integer-only algorithm: seedable, per-stream, no std distributions, no libm in its integer paths; for a unit-circle sample use an engine-side deterministic method or document that it is not libm-free); add an explicit seed API to the existing Random (e.g. Random.Seed(n) in Lua + C++) that, when called, switches Random.* to the deterministic generator; with no seed call behaviour is EXACTLY as today (random_device). (B4) Lua: `luai_makeseed` and the math.random seed currently come from time(NULL)/addresses (lstate.c:71-79, lmathlib.c:628-661, our build of external/lua): make both controllable from a project/engine setting `determinism.luaSeed` (unset = today's behaviour: randomised hash seed, time-seeded math.random; set = constant hash seed and math.random seeded from it). Think through and document the security note: a constant hash seed makes hash-flooding of string-keyed Lua tables possible, so it must stay OFF by default and the editor host must not enable it implicitly. Tests: same seed gives identical Random.* sequences and identical math.random sequences across two LuaRuntime instances in one process; pairs() order over a string-keyed table is identical across two states when the seed is set (and the test does not assert anything about the unset case). Docs: describe the setting and the 'no pairs() over gameplay state unless seeded' guidance. NOTE: sec-lua-budget (#159) is now MERGED: build on it: the Lua runtime now has LuaRuntime::RunChunk, LuaBudgetScope, the sandbox prelude (xpcall wrapper) and LuaPanicRepair; set the seeds where the state is created, without touching the budget/panic machinery; run the existing LuaBudgetTest suite unchanged. Also note the determinism hazard class found by #161: C++ does not fix function-argument evaluation order, so in any code or test you write, never call a stateful function (random, counter) more than once inside one argument list or operator expression.

## Allowed paths (regex, repo-relative)
- ^math/
- ^engine/
- ^external/lua
- ^cmake/
- ^CMakeLists\.txt$
- ^tests/
- ^docs/

## Acceptance
- With no seed call/setting, behaviour is unchanged
- Seeded Random and math.random sequences reproduce across two Lua states
- Lua hash seed constant only when the setting is set; security note documented
- Docs describe the API and the setting
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: default path untouched, how luai_makeseed is overridden in our vendored Lua build is sound (compile definition vs patched file: is the local patch recorded in THIRD_PARTY_NOTICES like the sol2 patches?), the seed is read before the first lua_State, tests do not depend on platform libm.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs det-random-lua` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-determinism-random-lua` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/det-random-lua/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-determinism-random-lua --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/det-random-lua/dev.json: {"pr":<number>,"branch":"feat-determinism-random-lua","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-determinism-random-lua` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
