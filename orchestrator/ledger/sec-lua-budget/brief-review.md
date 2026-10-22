# Review sec-lua-budget: Editor host security S4: Lua instruction budget and memory cap on every Lua state; Application.Quit refused on the editor host (audit F3 part a)
Read-only. Do NOT edit files, push, or comment on GitHub. Repo C:/Method Notes Obsidian/N2Engine. First `git fetch`, then review `git diff origin/master...origin/sec-lua-budget` (read C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-budget/dev.json for the PR number and the dev's deviations; if C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-budget/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...` from inside C:/Method Notes Obsidian/N2Engine.

## Plan under review
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

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; GL/GPU code CI can't run (say so); regressions. Say nothing about style.

## Output
Overwrite C:/Method Notes Obsidian/orchestrator/ledger/sec-lua-budget/review.json: {"head":"<origin/sec-lua-budget sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
