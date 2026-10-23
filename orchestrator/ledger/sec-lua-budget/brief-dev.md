# Task sec-lua-budget: Editor host security S4: Lua instruction budget and memory cap on every Lua state; Application.Quit refused on the editor host (audit F3 part a)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-lua-budget.

## Issue / plan
Source: the security audit C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md. Read finding F3 and section 6 items 4 and 5 fully first. Line numbers below are from origin/master at a1aa1005; re-find the code at current master.

THREAT CLOSED (F3 part a, High, authenticated editor A3).
- Project Lua runs on the edit host whenever a scene loads, and also when a loaded script is rewritten. The path is LuaComponent::Deserialize -> SetScript -> LoadScriptInstance -> LuaRuntime::RunSource (engine/src/scripting/LuaComponent.cpp ~564-599, ~67-160), plus hot reload through ReloadModule.
- The editor commands that reach it: LoadScene, OpenSceneFile, RestoreSceneSnapshot, the WritePlaySnapshot validation, and WriteTextAsset hot reload.
- The Lua state has no instruction limit and no memory limit. There is no lua_sethook, and sol::state uses the default allocator (engine/src/scripting/LuaRuntime.cpp ~39-71; `sol::state _lua` in LuaRuntime.hpp ~19).
- `Application.Quit` is bound (engine/src/scripting/bindings/LuaApplicationBindings.cpp ~13, `"Quit", &Application::Quit`), and the host loop exits on app.IsQuitRequested() (editor-server/src/Host.cpp ~538).
- Attack: an editor writes `while true do end`, or `local t={} while true do t[#t+1]=string.rep('x',1e8) end`, or `Application.Quit()` into a script, puts it on a LuaComponent and sets that scene as the startup scene. The host then hangs, runs out of memory, or exits cleanly with code 0, so systemd does not restart it. It does so again on every restart.
- This item adds resource limits and the Quit refusal. It does NOT change when scripts execute: not running scripts at edit-mode load time is a separate design (audit item 5), so do not touch it.

LUA STATES. At a1aa1005 the engine creates exactly one Lua state: the process-wide LuaRuntime singleton's sol::state. The sandbox opens only base, math, string and table, so there is no coroutine library and no extra threads. Grep again for lua_newstate, luaL_newstate, sol::state and lua_newthread in engine/ and editor-server/. Every state the engine creates gets the limits; tests that only borrow LuaRuntime::Instance().GetState() do not create one.

SCOPE.
(1) Instruction budget.
- Install a count hook with lua_sethook(L, hook, LUA_MASKCOUNT, N), where N is a granularity such as 1000 to 10000 instructions so the overhead stays negligible.
- The hook charges N against a remaining-instructions budget. When the budget runs out it raises a Lua error with a recognisable message ('script exceeded its instruction budget of X').
- The budget is reset at each OUTERMOST entry from C++ into Lua. An entry nested inside a running Lua call (a binding that calls back into Lua, or require inside a chunk) must not refill it. Use a depth counter in an RAII scope, for example LuaBudgetScope.
- Wrap EVERY C++-to-Lua entry: LuaRuntime::RunSource and RunFile; LoadScriptAsModule and ReloadModule; the require implementation; LuaComponent::CallLuaMethod, which covers OnAttach, OnUpdate, OnFixedUpdate, OnLateUpdate, OnDestroy, OnEnable, OnDisable, OnApplicationQuit, the collision, trigger and mouse callbacks and every other lifecycle method; script-instance creation and field extraction or injection that may call metamethods (ExtractSerializableFields, InjectFieldsIntoScript, SetEditorFields, Deserialize); MakeScriptCallback in engine/include/engine/scripting/ScriptCallback.hpp, which covers events, input Subscribe and UI AddOnClick; reload callbacks; and any other sol::protected_function or sol::function call site. Find them with grep.
- Default budget: 50,000,000 instructions per outermost entry, roughly 0.1 to 0.5 s of PUC Lua.
- Before settling the defaults, read the lua_project demo scripts (assets/scene.lua is about 33 KB and builds the whole demo scene; also the station scripts) and the Lua tests, and confirm they use far less. If a demo script genuinely exceeds the budget, fix the default rather than the demo, unless the demo is clearly wasteful.
- A script can catch the budget error with pcall or xpcall. Make that ineffective:
  - after the budget runs out, keep raising, for example by setting the hook count to 1 and raising on every hook call until the outermost scope ends;
  - make the sandbox's pcall and xpcall re-raise the budget error (the prelude in the LuaRuntime constructor already wraps load the same way).
  - Test both.

(2) Memory cap.
- Create the state with a custom lua_Alloc. sol::state has a constructor that takes a panic function, a lua_Alloc and a user pointer, and sol::default_at_panic is the panic function to pass.
- The allocator tracks the bytes the state uses and refuses (returns NULL) any allocation that would push the total past the cap. Lua then raises LUA_ERRMEM, which the existing protected calls catch.
- Watch the lua_Alloc contract: when ptr is NULL, osize is a type tag, not a size, so it must not be subtracted. A shrink or a free always succeeds.
- Default cap: 256 MiB for the whole state. All scripts share the one state, so say that in the docs.
- The cap must be changeable after construction (the allocator reads it from its user data) so tests can lower it.
- The engine's own bindings registration must fit comfortably. Measure the baseline in a test and log it at Debug level if that helps.

(3) Configuration.
- An engine API, for example `LuaRuntime::SetLimits({instructionsPerEntry, maxBytes})` plus a getter. The value 0 means unlimited, for tools that need it.
- Documented defaults applied in every build (game, play child, edit host).
- Optionally, HostOptions flags such as `--lua-instruction-budget <n>` and `--lua-memory-mib <n>`, only if they are trivial to add. If you add them, test them in HostOptionsTests and document them.

(4) On exhaustion (instructions or memory).
- The Lua error is caught by the existing protected call path: CallLuaMethod's result.valid() check, RunSource's protected result, and MakeScriptCallback.
- Log it ONCE per component, or once per chunk name for non-component entries, not once per frame.
- Mark that LuaComponent as faulted: skip its lifecycle calls until its script is set again or reloaded, which clears the flag. A looping OnUpdate must not burn the full budget every frame. Document this.
- The engine and the host keep running and keep answering commands.

(5) Application.Quit on the edit host.
- Find how the host and edit mode are represented. Application has _quitRequested and IsQuitRequested (engine/include/engine/Application.hpp ~41, ~100) and static Quit() (Application.cpp ~430-437), but I found no editor-host flag at a1aa1005.
- Add the minimal one, for example `Application::SetScriptQuitAllowed(bool)` or `SetEditorHost(bool)`. N2EditorHost sets it during startup (Host.cpp, before any scene loads, including the startup scene at ~360-372) when it is an EDIT host, that is without --play.
- The --play child is a running game that the editor launched, so a script's Quit there keeps today's behaviour and ends the play session. Document that choice.
- The Lua binding checks the flag. When Quit is not allowed it logs one Warn line ('Application.Quit is ignored on an editor host') and does nothing. C++ callers of Application::Quit are unaffected.

TESTS. They go in tests/engine/scripting/LuaRobustnessTests.cpp, or a new LuaBudgetTests.cpp registered in tests/engine/CMakeLists.txt, plus an editor-server project test. Use a lowered budget and cap through the API so the tests are fast; no test may take more than about a second.
- `while true do end` in OnUpdate is aborted with the budget error, logged once over several ticks. The component is marked faulted, and other components and the engine keep running.
- The same loop at the top level of a module chunk run by RunSource or LoadScriptAsModule returns an error.
- An infinite loop inside pcall, and inside xpcall with a message handler, is still aborted.
- A memory bomb (string.rep beyond the cap, and a growing table of strings) is aborted with a memory error, and the state stays usable afterwards: a normal script runs.
- Normal scripts are unaffected. The existing Lua tests pass unchanged, and a loop of, say, 1,000,000 iterations under the default budget succeeds.
- A nested entry (a Lua callback fired from inside a Lua call) does not refill the budget.
- Application.Quit from Lua with the editor-host flag set leaves IsQuitRequested() false and logs a warning. Without the flag (a game) it sets IsQuitRequested() true. Reset the quit state after the test.
- Editor server (tests/editor-server/project): LoadScene with a LuaComponent whose script loops forever at load time returns, and the host answers the next command. The same with a script calling Application.Quit: the host keeps serving.

DOCS.
- docs/scripting-lua.html: add a section next to #sandbox on the budgets (the defaults, the reset per outermost entry, pcall not escaping, the faulted component, the one shared state, and what is NOT limited). Work done inside a single C function is not counted by the instruction hook, for example a pathological string.find pattern, so say so.
- docs/editor.html security notes: say that 'editor' still means 'can run sandboxed code on the host', and that it is now bounded. Also that Quit is refused on the edit host, and that the edit-mode load-time execution is unchanged (the separate design).
- docs/application.html: the new flag.
- docs/testing.html: rows for the new tests.

OUT OF SCOPE.
- Changing when scripts run in edit mode (audit item 5).
- A separate scripts right or --allow-editor-lua.
- Protocol changes.
- The frontend.
- lua_project/: change it only if a demo script genuinely exceeds a sane budget.

CI. Nothing builds locally; CI is the build. There are 8 checks, and the required check is `linux (Release, x86_64, Clang)`. Lua runs in every leg's tests, and the play child smoke test runs lua_project-style scripts, so all legs must be green.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/
- ^tests/
- ^docs/
- ^lua_project/

## Acceptance
- All Tests-workflow jobs green (8 checks; required linux (Release, x86_64, Clang))
- Every engine-created Lua state has a count-hook instruction budget reset per outermost C++->Lua entry and a capped lua_Alloc, with documented generous defaults configurable via an engine API
- Budget/memory exhaustion is a caught Lua error, logged once per component, cannot be swallowed by pcall/xpcall; the engine keeps running
- Application.Quit from Lua is refused (one warning) on the edit host and still works in a game and the play child
- Tests: infinite loop in OnUpdate and in a chunk aborted, string.rep memory bomb aborted, normal scripts and the existing Lua tests unaffected, Quit refused/allowed; no change to when scripts execute; no protocol change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive (audit F3a). The reviewer must try to break it: (1) find a C++-to-Lua entry without a budget scope (grep every sol call, metamethod-triggering table access such as __index on a script's table read by ExtractSerializableFields, reload callbacks, event, input and UI callbacks); (2) escape the budget with pcall, xpcall, an xpcall message handler, error objects with __tostring or __gc metamethods, or a nested entry that refills it; (3) burn CPU inside a single C call, which the count hook cannot see (string.rep below the cap, string.find or gsub with pathological patterns, table.concat or table.sort with a Lua comparator, though the comparator is counted), and confirm the docs list what is not covered; (4) check the allocator against the lua_Alloc contract (osize is a type tag when ptr is NULL, frees always succeed, accounting never underflows), and what happens when an allocation fails inside sol or unprotected code: the panic path must not abort the host; (5) check that Application.Quit is refused on the edit host from every Lua path and still works in the --play child and in games; (6) check that defaults do not break lua_project or any existing test, and that test budgets are lowered through the API, not by changing the defaults. Not done here, by design: running script chunks at edit-mode deserialisation remains (audit item 5, a separate design). Expect small merges with sec-host-dos-limits in Host.cpp, HostOptions and docs; may run in parallel with it and with sec-scene-depth-cap.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-lua-budget` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-lua-budget` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-budget/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-lua-budget --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-budget/dev.json: {"pr":<number>,"branch":"sec-lua-budget","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-lua-budget` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
