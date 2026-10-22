# Task gcc-sol2-lambda-collision: Fix the sol2 lambda metatable-name collision under GCC so GCC-built hosts run Lua correctly
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-gcc-sol2-lambda-collision.

## Issue / plan
Recommended first by Claude and started on the user's instruction 2026-10-13 ("continue and start with your recommended actions"). Problem: sol2 (vendored, external/sol2/include/sol/sol.hpp, 3.2.3) names the metatable of a bound function object from ctti_get_type_name_from_sig(__PRETTY_FUNCTION__) (~line 8978). GCC prints every lambda of the same signature in one function as the same "<lambda(int)>", so two lambdas with one signature share a metatable and Lua calls the wrong one: on the GCC 14 leg 3 tests fail (LuaLayerTest.NamesAndMasks, LuaPickingTest.MouseFunctionsWithoutAWindow, LuaPickingTest.RaycastBindingsHandleBadInput; e.g. Layers.MaskOf ran LayerToName). The Clang and MSVC builds are correct. This is a real runtime bug for any GCC-built host, not a test problem. Goal: GCC-built Lua bindings behave identically to Clang/MSVC. Approach, in order of preference (pick one, explain why): (1) a minimal, marked patch to the vendored sol2 type-name path so the name is unique per lambda on GCC (for example derive it from typeid(T).name(), whose mangled name is unique per lambda, only when the type is a closure type, or a per-type counter/address that is stable within the process), leaving the Clang/MSVC paths byte-identical; add a marker comment like the existing optional<T&>::emplace patch (what, why, upstream version 3.2.3, re-apply on re-vendor) and check whether upstream sol2 already has a fix or macro for this and use it if so; (2) if (1) is impractical, make the engine's binding lambdas distinct types (named functors or free functions) in engine/src/scripting/bindings, but only if the set is small and a test guards against reintroducing the collision. Add a regression test that binds two same-signature lambdas in one function and checks each calls its own body (runs on every compiler). The GCC 14 leg in .github/workflows/tests.yml must go green; once it passes with all 3 tests, you may remove continue-on-error and the 'informational - known sol2 bug' wording from the leg's name and comments (it must stay NOT required: the user sets branch protection) and update the doc sentences that say GCC-built hosts are unsupported (docs/testing.html, docs/editor.html Linux section). Do not touch the required Clang job's name or steps. Iterate on CI; the first GCC run after a change takes ~10 min.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^external/sol2/include/sol/sol\.hpp$
- ^docs/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green including the GCC 14 leg with all Lua tests
- A regression test for same-signature lambdas that passes on Clang, GCC and MSVC
- The sol2 patch (if any) is marked in the vendored file and documented
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Touches vendored code and the Lua binding layer: the reviewer checks that the Clang/MSVC paths are unchanged (diff of the preprocessed paths by inspection), that names stay stable and unique (no collisions with distinct non-lambda types), and that no behaviour changes for existing Lua scripts or saved scenes. Unverified: nothing GPU related; a real GCC-built host run is covered by the GCC leg's smoke tests only if the leg runs them (add the two Python smoke steps to the GCC leg if cheap).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs gcc-sol2-lambda-collision` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-gcc-sol2-lambda-collision` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/gcc-sol2-lambda-collision/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-gcc-sol2-lambda-collision --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/gcc-sol2-lambda-collision/dev.json: {"pr":<number>,"branch":"fix-gcc-sol2-lambda-collision","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-gcc-sol2-lambda-collision` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
