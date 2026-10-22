# Task net-c1-deterministic-math-lua: Netcode C1 (= Jolt J8): engine-owned deterministic sin/cos/atan2/acos/asin/exp/log used by math and Lua, a per-tick floating-point-environment guard, deterministic Lua shims, Ordered.pairs; exact-value tests on all legs
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-c1-deterministic-math.

## Issue / plan
Netcode design C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (Track C, C1) and physx-limitations-and-determinism.md blocker B8 and decision D7 (user: ordered-iteration helper for Lua, the Lua seed switch is already merged #172), investigation C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md section 3 (strict float engine-wide, engine-owned trig, per-tick FP-environment guard: P7 ACCEPTED). Read those sections, the merged determinism S1 work (docs: determinism, N2ENGINE_STRICT_FP, math.simdTier, DeterministicRandom, ordered containers), engine/math (Quaternion.cpp/Matrix.hpp uses std::sin/cos/acos/asin/atan2), the Lua math/Random bindings (LuaMathBindings.cpp), and A1's validation requirement 'engine-owned trig (C1)' (engine/src/network/NetworkValidation.cpp) FIRST. SCOPE: (1) Math::Sin/Cos/Tan/Atan2/Acos/Asin/Exp/Log (and Pow only if needed) as ENGINE-OWNED deterministic implementations: polynomial/minimax approximations with a fixed operation order, float and the engine's other precisions only as used, no std:: transcendental calls, no FMA contraction (they are compiled under the strict-FP profile, add a static check that N2_STRICT_FP is on when the deterministic implementations are selected? : they must be bit-identical across MSVC, GCC, Clang, arm64 and wasm in ALL builds regardless of the flag by writing the code so contraction cannot change results (explicit operation ordering with volatile-free techniques: use the repo's approach from #162: read it), accuracy documented (max ulp error measured by a test against a high-precision reference computed in the test with long double or a series), special values (0, +-pi/2, large arguments with a documented reduction, NaN/inf) defined; (2) replace every std::sin/cos/acos/asin/atan2 (and exp/log where simulation-relevant) in engine/math, the engine's Quaternion/Matrix/Transform code and the Lua bindings with the engine-owned ones: audit with grep and list every call site you changed and every one you intentionally kept (rendering-only code, editor tools, text/UI layout may keep std if it cannot influence simulation: justify each); results of existing math tests may shift in the last ulp: update expected values deliberately with the old/new values listed in the PR and docs; (3) a per-tick FLOATING-POINT ENVIRONMENT GUARD (RAII): sets and verifies the FP control state (rounding mode round-to-nearest, FTZ/DAZ policy the same on every platform: choose flush-to-zero OFF everywhere unless the investigation says otherwise, x87 not used on x64/arm64) at the start of each fixed tick in network/deterministic simulation mode and restores it; unit-tested by deliberately perturbing the control word in a test; no effect in the None mode; (4) Lua deterministic shims: math.sin/cos/tan/asin/acos/atan/exp/log and math.pow-like operators exposed to Lua use the engine-owned versions when the project's determinism settings are on (read how determinism.luaSeed switches behaviour today: follow that gate) and are otherwise unchanged; Ordered.pairs(t) helper in Lua returning keys in a deterministic order (documented: array part in order then other keys sorted by type then value; error on mixed uncomparable keys per a stated rule), plus docs for authors; (5) A1's validation requirement 'engine-owned trig' becomes MET (NetworkValidation) and its test/docs/preset detail text updated; (6) TESTS (engine unit tests on all legs incl. arm64 and web): exact-value tests: a table of inputs with the exact expected bit patterns computed once and checked on every leg (identical across legs is the point), accuracy bounds, special values, monotonicity/periodicity sanity, FP guard perturbation test, Lua shim and Ordered.pairs tests, the A1 requirement change; (7) DOCS: docs on math/determinism (find them), docs/scripting-lua.html (shims, Ordered.pairs), docs/testing.html. Nothing builds locally for the engine; CI (11 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^math/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$

## Acceptance
- Engine-owned deterministic transcendental functions used by math and Lua; every std call site audited (changed or justified)
- Exact-bit-pattern tests identical on all legs; accuracy documented and tested
- Per-tick FP environment guard tested; Lua shims and Ordered.pairs; A1 requirement now met
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: numerical correctness (argument reduction, polynomial accuracy, special values), no hidden std transcendental left in simulation paths, contraction/ordering safety on every compiler, exact-value tests meaningful and identical across legs, guard restores state on all paths, None behaviour unchanged, Lua gate correct, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-c1-deterministic-math-lua` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-c1-deterministic-math` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-c1-deterministic-math-lua/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-c1-deterministic-math --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-c1-deterministic-math-lua/dev.json: {"pr":<number>,"branch":"feat-net-c1-deterministic-math","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-c1-deterministic-math` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
