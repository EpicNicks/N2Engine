# Task sec-lua-registry-reserve: Lua limits follow-up: no std::terminate from noexcept sol reference copies at the memory cap (registry reserve)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-lua-registry-reserve.

## Issue / plan
Follow-up of sec-lua-budget (PR #159). Read docs/scripting-lua.html, resource limits, the known limitation tracked by this item, and engine/src/scripting/LuaPanicRepair.cpp / LuaRuntime.cpp Panic first.

PROBLEM. In sol2, creating or copying a Lua reference (sol::reference/table/object/protected_function construction, copy, stack-to-reference conversion, sol::make_object) is noexcept and calls luaL_ref. If the registry must grow for it while the Lua state is at the memory cap and no protected call is active, Lua raises a memory error, the panic function throws sol::error, the exception meets the noexcept frame and the process ends with std::terminate. Precondition: an authenticated editor deliberately fills the state to the cap and leaves the registry without a free slot, then triggers one of the paths (scene load, hot reload via WriteTextAsset, a collision). Consequence: host crash, restarted by the supervisor; no memory corruption.

SITES (re-find at current master): LuaComponent::CreateScriptInstance `scriptClass = result[0]`; LuaComponent::LoadScriptInstance `_scriptInstance = instance`; LuaComponent::RetireScriptInstance `sol::table previous = _scriptInstance`; LuaComponent::GetEditorFields (the instance copy and the iteration over SerializableFields); LuaRuntime::LoadScriptAsModule `moduleTable = result[0]` and `_loadedModules[moduleName] = moduleTable`; Bindings::CollisionToLua / TriggerToLua (the reference sol::make_object creates). Grep again for any other reference created or copied outside a protected call (sol::table/sol::object/sol::protected_function copies, as<>/get<> into reference types, iterators) on paths that run outermost (no Lua call on the stack).

SCOPE.
- Make every such site unable to fail: either do the copy inside LuaRuntime::ProtectedCall (careful: a stack-based object such as a protected_function_result cannot be read from inside the trampoline, its indices belong to the outer frame; move the value to the registry first in a protected way, or pass it as the pcall argument), or reserve registry free-list slots / grow the registry in a protected call just before each unprotected copy so luaL_ref reuses a free slot and allocates nothing. Consider pre-growing the registry when the runtime is created and a small helper (e.g. LuaRuntime::ReserveReferences(n)) called by each site.
- Keep every other behaviour unchanged (budget, cap, faulting, logging, Quit).
- Tests that make the scenarios deterministic: e.g. a test-only allocator hook in the capped lua_Alloc that fails the Nth allocation or every growth of a given size, so a registry growth at each listed site is forced; each scenario must log/fault and keep the process running. No test may terminate the test binary.
- Docs: replace the known-limitation paragraph in docs/scripting-lua.html with what the code then does; docs/testing.html rows for the new tests.

OUT OF SCOPE: changing when scripts run at edit-mode load (audit item 5); protocol; frontend. ADDITIONAL (from the final review of #159, docs lows to include in this item): (1) the terminate-site list in docs/scripting-lua.html and in this item is incomplete: it omits the `raw_get<sol::object>("__gc")` call on the script class in LuaComponent::CreateScriptInstance (a reference created outside any protected call whenever __gc is not nil; the script chooses the value): grep again for EVERY luaL_ref-creating sol copy or construction outside a protected call (sol::object/table/function/reference copies, make_object, raw_get<sol::object>, operator[] results stored, std::function captures of sol refs) and cover all of them; (2) the Known limitation / platform sentence 'every platform the engine builds for (MSVC, and Clang and GCC on Linux)' omits the web build (the web job runs these tests and is green): fix the docs. After this item the registry-copy limitation should be CLOSED: update docs/scripting-lua.html accordingly (what is protected now and what still is not).

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- All Tests-workflow jobs green (required linux (Release, x86_64, Clang)), the web job included
- No listed site (and none found by a fresh grep) can create a registry reference outside a protected call without a reserved free slot; failure at the cap is a caught, logged error
- Deterministic tests force a registry-growth failure at each listed site and the test binary keeps running
- No other behaviour change; docs updated in the same PR
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security hardening (audit F3a follow-up). The reviewer must try to find a remaining reference creation outside a protected call on an outermost path, and check the test-only allocator hook cannot be enabled in a shipped build.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-lua-registry-reserve` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-lua-registry-reserve` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-registry-reserve/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-lua-registry-reserve --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-registry-reserve/dev.json: {"pr":<number>,"branch":"sec-lua-registry-reserve","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-lua-registry-reserve` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
