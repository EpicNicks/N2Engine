# Security design: Lua in edit mode (F3b) and bounded pre-auth slots (F1/F2/F6) (proposal, 2026-10-15)
Tags: [V] verified by reading N2Engine `origin/master` at `a1aa1005` (the checkout was identical and clean); [M] assumption or general knowledge, not checked in this code. Paths are relative to N2Engine/. Source: `orchestrator/proposals/editor-host-security-audit-2026-10-15.md` (findings F1, F2, F3, F6; fix items 3, 4, 5, 8). Item names used below: item 1 = pre-auth accounting plus a 2 s budget; item 2 = S2 `sec-host-dos-limits` (idle and write deadlines); item 4 = S4 Lua budgets; item 5 = this Part 1; item 3 = this Part 2; item 8 = rate-limiter fixes. Design only: nothing was built, run or changed.

---

# PART 1 (F3b, item 5): Lua in edit mode

## 1.1 What runs today, and when [V]

The engine's edit-mode rule (318d4a8, `Scene::SetEditMode`) means "built, never attached":
- **No OnAttach and no Update.** The edit host never calls `Application::Tick`, which is the only caller of `ProcessAttachQueue` (`engine/src/Application.cpp:320`). Its loop only calls `ProcessCommands` (`editor-server/src/Host.cpp:538-561`).
- **No OnEnable/OnDisable on active-state changes** (`GameObject.cpp:144-151`).
- **No callbacks on RemoveComponent** (`GameObject.cpp:527-543`).
- **No OnApplicationQuit.** `Scene::OnAllActiveComponents` walks only `_components`, the attached ones (`Scene.cpp:403-423`, `:532`).

**But the script's top-level chunk does run, in the one global `sol::state`, with every engine binding.** `LuaComponent::SetScript` → `LoadScriptInstance` → `CreateScriptInstance` → `LuaRuntime::RunSource` (`LuaComponent.cpp:67-97`, `:109-145`, `:147-195`; `LuaRuntime.cpp:73-77`). `SetScript` is reached from `Deserialize` (`:564-605`) and from `SetEditorFields` when `scriptUUID` changes (`:829-851`).

The top-level chunk therefore runs on each of these paths:

| Path | Runs per LuaComponent | Evidence |
|---|---|---|
| `LoadScene` (client JSON) | **2-3 times**: `Scene::FromJSON` for validation, again in `SceneManager::AddScene(json)` (`SceneManager.cpp:225-234`), and again when `ProcessAnyPendingSceneChange` rebuilds from the stored data (`:310-312`). It is 2 when the name already exists (`UpdateScene` + `LoadScene`). | `EditorServer.cpp:3559-3581` |
| `OpenScene` / the startup scene at boot | once (`FromJSON`, then `AddScene(unique_ptr,true)` uses that instance) | `EditorServer.cpp:6520`, `:6448-6457`; `Host.cpp:360-373` |
| `WritePlaySnapshot(file)` validation | once, then thrown away | `EditorServer.cpp:4154` |
| Undo/redo and snapshot restore (`RestoreSceneSnapshot`) | once per component in the whole scene | `EditorServer.cpp:5865` |
| Undo of a delete (`RestoreSubtree`) | once per restored component | `EditorServer.cpp:1247` |
| `DuplicateEntity` (`InstantiatePrefab`) | once per copied component | `EditorServer.cpp:4714` |
| `AddComponent` with values (`AddComponentFromValues`); `ApplyComponentState` | once | `EditorServer.cpp:1317-1350`, `:1386-1416` |
| `SetComponentFields` that changes `scriptUUID` | **twice**: the scratch copy (`:668-679`) and the real component (`:721`), plus a third time on the restore-on-failure path (`:733`). A `scriptData`-only edit runs **no** chunk: it only calls `DescribeFields` and `InjectFieldsIntoScript` (`LuaComponent.cpp:853-878`). | `EditorServer.cpp:640-736` |
| Hot reload (`WriteTextAsset`, the asset watcher, `RescanAssets`) → `ReloadLoadedAsset` → `LuaRuntime::ReloadModule` | **1 + N**: once as a module (`LoadScriptAsModule`, even if nothing `require`s it), then once per component through its reload callback (`ReloadScript` → `LoadScriptInstance`) | `EditorServer.cpp:5299-5321`; `LuaRuntime.cpp:246-265`; `LuaComponent.cpp:918-939` |
| `require` from any of these chunks | once per module, cached in `_loadedModules` | `LuaRuntime.cpp:158-179` |

**Script callbacks that DO run in edit mode (new finding, not in the audit)** [V]:
- `RunDestroyCallbacks` is not gated on edit mode (`Component.cpp:19-44`).
- `DestroyEntity` runs it through `ProcessDestroyed` → `CallOnDestroyForGameObject`. The code says so: "runs component (incl. Lua) callbacks" (`EditorServer.cpp:3745-3747`; `Scene.cpp:615-690`).
- Replacing the open scene (OpenScene, LoadScene, undo restore) runs it through `ProcessAnyPendingSceneChange` → `Clear()` → `ClearImpl(false)` (`SceneManager.cpp:319-322`; `Scene.cpp:550-580`).
- `LuaComponent::OnDisable` and `OnDestroy` then call the script's `OnDisable`/`OnDestroy` whenever an instance exists (`LuaComponent.cpp:404-427`).

So a deleted object, or a scene switch, runs script code with full bindings. The C++ components' destroy callbacks in edit scenes are tested on purpose (`tests/editor-server/project/EditorInspectorTests.cpp:143-144`, `:218`). The Lua side is incidental.

**What does NOT run Lua** [V]:
- `GetLuaFields` reads the existing instance's `SerializableFields` (`EditorServer.cpp:5053-5079`, `LuaComponent.cpp:699-820`).
- `GetComponentTypes` → `ComponentRegistry::Describe` builds a LuaComponent with no script, on an object in no scene, and caches it (`ComponentRegistry.cpp:28-48`).
- A `scriptData`-only inspector edit.
- `Serialize`, which writes `_scriptData` only (`LuaComponent.cpp:543-562`).

**What the top-level chunk can do on the edit host** [V]:
- Everything bound in `LuaRuntime::RegisterBindings` (`LuaRuntime.cpp:102-125`), including `Application.Quit` (`LuaApplicationBindings.cpp:12-13`), `GameObject.Create`/`Destroy` and `SceneManager.LoadScene` (`LuaCoreBindings.cpp:77`, `:142`, `:223-234`).
- It can mutate the scene being edited, or queue a scene change, in the middle of a deserialisation.
- It can write globals shared by every script.
- It runs with no instruction or memory limit (no `lua_sethook` or `lua_newstate` in the repo: grep).

## 1.2 What the editor actually needs from a script in edit mode [V]

Only the **declarations**: the string keys of the returned table's `SerializableFields`, and for each one:
- an optional `type` string;
- a `default`: an integer or a float, a bool, a string, a `Vector3`, or nil.

They feed four places:
- `DescribeFields`, which is the inspector and `GetLuaFields` (`LuaComponent.cpp:699-820`);
- `ExtractSerializableFields`, the defaults written into `scriptData` (`:241-288`);
- the "keep only declared fields" rule when the script changes (`:166-183`);
- `ResolveReferences`, which reads `type` to bind `$ref` handles onto the instance (`:617-688`).

`ResolveReferences` is only useful to running code. In edit mode the reference stays as `{"$ref": uuid}` in `scriptData`, which is what is saved.

Nothing in `editor-server/` reads Lua globals, the instance's methods, or `CacheLifecycleMethods` flags [V: grep of `EditorServer.cpp` for Lua shows only `GetLuaFields`, `ReloadModule` and the script template].

The forms that must keep working:
- the documented forms (`docs/scripting-lua.html:274-292`);
- the tests (`tests/engine/scripting/LuaReflectionTests.cpp:57-95`: shorthand values, `{type=,default=}`, `default = Vector3(0, 1, 0)`, reference types, a non-string key);
- the generated template (`engine/src/scripting/LuaScriptTemplate.cpp:20-25`).

**Compatibility data point** [V]:
- No script under `lua_project/` declares `SerializableFields`, and no committed `.scene` uses a LuaComponent (`lua_project/scenes/viewport_check.scene` has none; `project.n2proj` has no startup scene).
- `lua_project` builds its scene from `scene.lua` at runtime.
- Its scripts do top-level work that a declaration pass must survive or skip: `require("scripts.SmokeStations")`, `89.0 * Math.DEG_TO_RAD` (`CameraController.lua:7-15`), `Quaternion.FromEulerAngles(...)` (`SoftBodies.lua:15`).
- External projects are unknown [M].

## 1.3 Options

### Option A: never run script code in edit mode; static parse of the declarations
A small C++ reader finds `<Name>.SerializableFields = { ... }` (and `return { SerializableFields = ... }`), and accepts only literals:
- numbers (with unary minus), strings, `true`/`false`/`nil`;
- nested `{ type = "...", default = ... }`;
- `Vector3(a,b,c)` / `Vector3.new(a,b,c)` with literal numbers.

Anything else (a local constant, arithmetic, a `require`d value) makes that entry "unknown". Unknown entries become `Json` kind with no default, and a warning is logged.

- Pros:
  - No VM at all on the edit host: F3's hang, OOM, Quit and scene mutation are gone from edit mode by construction.
  - Deterministic and cheap.
  - Hot reload becomes "re-parse".
- Cons:
  - A hand-written parser of untrusted text is itself attack surface. It must be iterative, with depth and size caps.
  - It reimplements a slice of the Lua grammar: long strings `[==[`, comments, escapes, `0x` and float formats, `;` separators.
  - It is incompatible with computed defaults (`speed = BASE * 2`, `default = Vector3.Up`, a shared `require`d table). Those lose their default or their kind in the inspector. Saved values survive, because they live in `scriptData`.
  - Edit-time and play-time can disagree about a field's kind.

### Option B: declaration-only evaluation in a throwaway, budgeted state with no engine bindings
For each distinct script source, cached by content hash, the editor:
1. creates a fresh `lua_State` (`sol::state` with a custom allocator [M: sol2 takes a `lua_Alloc`]);
2. opens only `base` (minus `load`, `dofile`, `loadfile`, `collectgarbage` and `print`), `math`, `table`, and a `string` without the pattern functions;
3. installs an inert `_ENV` fallback: any unknown global (Math, Quaternion, Input...) is a recursive stub that can be called, indexed and used in arithmetic, and `require` returns a stub;
4. defines `Vector3(x,y,z)` / `Vector3.new` as pure Lua returning a tagged value that C++ reads as a Vector3;
5. runs the chunk under an instruction-count hook and a byte-capped allocator;
6. reads `SerializableFields` from the returned table into a plain C++ `ScriptDeclaration` (`{name, type, default json}`), and closes the state.

The LuaComponent in edit mode holds the declaration, **not** a `sol::table` instance.

- Pros:
  - Compatible with nearly every real script: computed defaults work, and top-level engine calls become no-ops on stubs.
  - No engine binding is reachable, so there is no `Application.Quit`, no scene mutation and no shared globals.
  - The cost is bounded per script and evaluated once per distinct source (the cache), which also removes today's 2-3x and 1+N re-runs.
  - It reuses PUC Lua's real parser rather than a new one.
- Cons:
  - It still executes attacker-written Lua, only bounded. The residual risks are a PUC Lua 5.4.8 VM bug [M: rare without `debug` or bytecode; bytecode is already refused], and CPU spent inside C functions, which the count hook does not see (hence no `string.find`/`gsub`/`match` patterns in this environment; `string.rep` and `table.concat` are bounded by the allocator cap).
  - A script whose declarations depend on a real engine value (`default = Input.Something()`) gets a stub, so that entry becomes unknown.
  - `pcall` can catch a budget error. Once the budget trips, the hook must keep failing on every instruction (count 1 plus a line hook), so the outer chunk cannot continue.

### Option C: keep running chunks as today, plus the item 4 (S4) budgets
- Pros:
  - The smallest change; no editor feature changes.
  - It stops infinite loops and OOM.
- Cons:
  - Still runs with every binding in the shared global state: `Application.Quit`, `GameObject.Create`, `SceneManager.LoadScene` in mid-deserialisation, and global pollution.
  - It still re-runs 2-3x per load and 1+N per reload.
  - It still runs `OnDisable`/`OnDestroy` on delete and on scene switch.
  - The budget must be generous enough for real gameplay, so it is a weak cap for the edit host.
  - "Editor can run code on the host" stays true. Quit would still need its own guard (item 5's second half).

### Editor features that would change under A or B [V unless marked]
1. **Hot reload changes meaning on the edit host:** the new source is re-declared, not executed. `EditorAssetsTest.WritingALoadedScriptReloadsItInPlace` asserts "once as the module, once for the component" through globals (`tests/editor-server/project/EditorAssetTests.cpp:822-850`). It and `ADeletedAndRestoredScriptIsReloadedWithoutTouchingFreedMemory` (`:852-882`) must be rewritten to assert that the declaration changed and globals did not. `AScriptErrorOnReloadIsALogEventNotAnErrorResponse` (`:898`) keeps working, because a syntax error is still logged by the compile step. `WritingAScriptNothingHoldsRunsNothing` (`:884`) is unchanged.
2. **No Lua `OnDisable`/`OnDestroy` on delete or scene switch** on the edit host. This is a behaviour change, but it is consistent with "never attached, so nothing to undo" (`Scene.cpp:28-32`). C++ components are unaffected.
3. **A script that fails to declare** (syntax error, over budget, no table) keeps its `scriptData` untouched, and GetLuaFields lists the `scriptData` keys with kinds inferred from the JSON values (so nothing is lost or hidden). Its "keep only declared fields" step is skipped (keep everything) rather than dropping all fields. Today a failed load drops the instance (`LuaComponent.cpp:157-162`), and DescribeFields then shows only `scriptUUID`, so this is an improvement.
4. **The play host (`--play`) and the standalone game are unchanged:** they run scripts normally. Item 4 (S4) budgets apply there.

## 1.4 Recommendation (Part 1)

**Option B, with a process-wide execution policy, plus item 4 (S4) for the hosts that really run scripts.**

- **Policy.** `LuaRuntime::SetExecutionPolicy(DeclarationsOnly)` is set by `Host.cpp` when `options.playScene.empty()`.
  - An edit host never plays in-process: play mode is entered only at startup via `--play` (`Host.cpp:397-422`, `EditorServer.cpp:4189-4220`) [V].
  - The policy is per process because edit mode is set only *after* `Scene::FromJSON` has deserialised (`EditorServer.cpp:3590`, `:5875`, `:6455`), so a per-scene flag is too late [V].
- **In `DeclarationsOnly`:**
  - `LuaComponent::SetScript` asks a `ScriptDeclarationCache` (keyed by resource path plus source hash) and never touches the global state.
  - `ReloadModule` refreshes the cache entry and notifies components to re-read; it runs nothing.
  - `CallLuaMethod` is unreachable because no instance exists. A defensive `assert` plus an early return is enough.
- **Budgets for the declaration pass** (decision 2): 1,000,000 VM instructions and 16 MiB per script; source at most 4 MiB (already the text-asset cap); at most 256 fields read; strings in defaults up to 4 KiB.
- **Not Option A:** B keeps compatibility with computed defaults at the cost of a bounded VM run. A could be added later as a fast path for pure-literal scripts.
- **Not Option C:** it leaves the bindings, the re-runs and the destroy callbacks.
- **Also in this work:**
  - remove the duplicate builds in `HandleLoadScene` (validate once, then `AddScene(std::move(scene), true)`), which is good regardless;
  - `Application.Quit` becomes a logged refusal when the policy is not `Run`. It is unreachable then, but this guards future paths.
- **Docs:** "editor" no longer means "runs project code on the edit host". The `CommandClass::Write` comment (`EditorServer.hpp:527-529`) and `docs/scripting-lua.html#sandbox` change accordingly.

## 1.5 Migration and compatibility
- No file format change. Scenes keep `scriptUUID`/`scriptPath` plus `scriptData`, and saved values still win over defaults.
- No protocol change: GetLuaFields has the same shape.
- External projects whose scripts do engine work at top level keep working at play time. In the editor, such work is skipped.
- A field whose default came from a real engine call shows as `Json` (or the declared `type` if one is given). Release notes should say "declare `type =` for computed defaults".
- `lua_project` is unaffected, since it has no `SerializableFields` [V].

## 1.6 Tests (Part 1)
- **Engine unit, `ScriptDeclarationTests`:**
  - all forms in `LuaReflectionTests.cpp:57-95` give the same `FieldInfo`s as today, a regression oracle run under both policies;
  - `while true do end`, `pcall`-wrapped loops, and `string.rep('x', 1e9)` each fail within budget with a logged error and an empty declaration;
  - `Application.Quit()`, `GameObject.Create("x")` and `SceneManager.LoadScene(...)` at top level hit stubs: no object is created, no quit, no global is set in `LuaRuntime::GetState()`;
  - `require` gives a stub;
  - `string.find` is absent;
  - the cache hits on the same hash and misses after an edit.
- **Editor tests:**
  - LoadScene, OpenScene, `WritePlaySnapshot` and undo with a script that loops, quits or sets a global: the host keeps answering, the global is unset, and `IsQuitRequested` is false;
  - DestroyEntity of a scripted object runs no Lua `OnDestroy`;
  - the rewritten hot-reload tests assert the changed declaration;
  - the `EditorHistoryLuaTest` and `EditorInspectorProjectTest` suites stay green unchanged;
  - "a failed declaration keeps `scriptData`".
- **Play path:** the existing `LuaLifetimeTests`, `LuaTests` and `PlaySmokeTest.ps1` stay green under the `Run` policy.

## 1.7 Staged PR plan (Part 1)
1. **PR L1, engine.** `ScriptDeclaration`, the declaration evaluator (throwaway state, stub environment, count hook, allocator cap) and the cache. Unit tests only, nothing wired.
2. **PR L2, engine and editor-server.** The execution policy; LuaComponent declaration mode; `ReloadModule` in that mode; `Host.cpp` sets it; `HandleLoadScene` builds once; `Application.Quit` guard; test rewrites; docs (`scripting-lua.html`, `editor.html` role text, `serialization.html#reflection-lua`).
3. **PR L3 = item 4 (S4).** Instruction and memory budgets for the `Run` policy (play host and game) per entry point (chunk, each callback), with the same "trip and keep failing" hook. Can go in parallel with L1.

Order: L1 → L2. L3 is independent. L2 depends on F4/item 6 only if they touch the same lines of `HandleLoadScene` [M].

## 1.8 Decisions for the user (Part 1)
1. Option B (recommended), A, or C.
2. Declaration budgets: 1M instructions and 16 MiB per script (recommended), or other values.
3. Accept that Lua `OnDisable`/`OnDestroy` no longer run on the edit host on delete or scene switch.
4. No escape hatch (recommended), or an opt-in `--run-scripts-in-edit-mode` for local trusted use.
5. A failed declaration shows `scriptData` keys with inferred kinds (recommended), or only `scriptUUID` as today.
6. Keep the audit's separate "scripts capability for editors" idea deferred (recommended: B removes most of its reason on the edit host).

## 1.9 Risks (Part 1)
- **Stub environment surprises.** A script that branches on a stub (`if Input then ...`) takes a different path in the editor than at play. That is accepted, but should be documented.
- **C-level CPU** in allowed library functions not seen by the count hook. Mitigated by the trimmed `string` library and the allocator cap; `table.sort` with a Lua comparator is counted [M].
- **sol2 with several states.** The usertypes are bound only in the global state. The declaration state must not receive engine usertypes by mistake. Keep its setup in one function with a test that lists its globals.
- **The cache key must include the source text hash**, not mtime, or a write within the mtime granularity would serve a stale declaration.
- **Divergence** between the edit host's declarations and the play host's real instance. GetLuaFields on a play host still uses the instance, which is correct there.

---

# PART 2 (item 3, F1/F2/F6): bounded pre-auth slots

## 2.1 How the host serves connections today [V]
- **One network thread (`_serverThread`) and the main thread.**
  - `ServerLoop` (`EditorServer.cpp:2047-2173`) accepts, reads the peer, asks `RateLimiter::AdmitConnection` (`:2088-2107`), runs `BeginTls` inside `connectedAt + helloBudget` (`:2111-2126`, `:2175-2217`), `ReloadTokens(false)` (`:2132`), then `HandleClient` **to completion** (`:2140`).
  - It then enqueues cleanup on the main thread: `CloseEditGroups`, `ReleaseAll`, `FlushAutosave` (`:2156-2164`).
  - `listen(_listenSocket, 1)` (`:1550`).
- **`HandleClient`** (`:2229-2427`):
  - It reads frames with a deadline only while gated and pre-Hello (`:2244-2249`), and caps the payload at 64 KiB pre-Hello and 64 MiB after (`:2280-2291`).
  - It refuses anything but Hello pre-auth (`:2303-2310`) and answers Shutdown itself (`:2312-2354`).
  - It checks the per-user backoff **before** the token (`:2361-2371`, F6).
  - It then `Enqueue`s every command, **Hello included**, to the main thread and blocks on the future (`:2379-2390`; `CommandQueue.hpp:15-65`).
- **`HandleHello` runs on the main thread** (`:3004-3085`):
  - `Authenticate` (token table copied under `_tokensMutex`, `EditorServer.hpp:771-775`), then a version check;
  - side effects: `CloseEditGroups`, `_keys.ReleaseAll`, `_clientErrorCodes`, `SetIdentity`;
  - then ServerInfo.
- **Per-connection state lives in `EditorServer` members**, which assumes a single connection: `_peer`, `_tlsSession`/`_tlsSessionSocket`, `_identity` (mutex), `_closeConnection`, `_clientErrorCodes`, `_peerText` (mutex), `_helloTokenRefused`, `_clientConnected` (`EditorServer.hpp:779-821`, `:894`). `RateLimiter` has no lock: "network thread only" (`:788-794`).
- **I/O.** `Send` has no deadline (`:6833-6867`). `Receive` takes an optional one (`:6869-6908`). `WaitUntilReady` uses `select` with a 100 ms slice, services SIGHUP token reloads, and checks `_closeConnection` (`:6910-6941`, `PollIntervalMicroseconds` `:99`). `PostLog` is a fire-and-forget enqueue to the main thread (`:1634-1645`), so it is thread-safe but unbounded (F7).
- **TLS.** Mbed TLS is built with `MBEDTLS_THREADING_C` plus `_ALT` std::mutex callbacks (`editor-server/mbedtls-config/n2_mbedtls_user_config.h:4-5`, `EditorTls.cpp:192-223`). One shared `mbedtls_ssl_config` uses the CTR-DRBG as its RNG (`:626`). Concurrent handshakes on separate `ssl_context`s are supported under THREADING_C [M: the DRBG locks its own mutex in 3.6].
- **Test hooks:** `SetPeerAddressProvider`, `SetRateLimitClock` and `SetHelloTimeout` (`EditorServer.hpp:205-230`). `RateLimitServerTests.cpp` runs a real server over loopback with a fake peer and a fake clock and never sleeps (`:38-41`).

## 2.2 Goals and non-goals
Goals:
- **(G1)** No unauthenticated connection can delay or block an authenticated session's commands.
- **(G2)** Pre-auth work is bounded in threads, memory, time and CPU.
- **(G3)** One authenticated session at a time stays the model. Engine state is single-threaded, and commands still run on the main thread through `CommandQueue`.
- **(G4)** An idle or stuck authenticated client cannot hold the host forever. Item 2 deadlines plus a hand-off policy.
- **(G5)** A legitimate client gets in within about 2 s against an attacker with one or a few addresses.

Non-goals (out of scope): PROXY protocol or trusted proxies; per-IP connection caps beyond the bounded table below; botnet-scale floods (firewall, SSH tunnel); concurrent multi-user editing; IPv6 listening.

## 2.3 Options

### Threading shape
| | T1: fixed pool of N pre-auth worker threads (recommended) | T2: one pre-auth event loop (`poll` over N non-blocking sockets) | T3: thread per connection, capped by a semaphore |
|---|---|---|---|
| Code reuse | High: `BeginTls`/`Receive`/`WaitUntilReady` move into a `Connection` and keep their blocking-with-deadline style | Low: incremental frame parsing and a TLS step state machine per socket (the TLS steps are already non-blocking, `EditorServer.cpp:2192-2216`) | High |
| Locks | RateLimiter and slot table need a mutex | The limiter stays single-threaded | As T1 |
| Bounded? | Yes (N threads, created once) | Yes (1 thread) | Yes if the semaphore is right; thread churn under a flood |
| TLS CPU spread | Up to N cores at once | 1 core: naturally rate-limits handshakes | Up to N |
| Risk | Moderate | Higher (new I/O code) | Moderate, plus thread create/join churn |

### Second authenticated client while one is active
| Policy | Pros | Cons |
|---|---|---|
| P1: reject with "busy" | Simple, predictable, no hijack | A crashed client's half-open session blocks its own reconnect until item 2's idle timeout |
| P2: queue (hold the authenticated connection until the session ends) | Today's implicit behaviour (backlog) | It holds a pre-auth slot, so attackers holding tokens can fill the queue, and it needs a queue deadline anyway |
| P3: preempt by higher role (admin > editor > viewer); equal or lower gets busy | Fixes F2's "viewer locks out admin" without waiting for timeouts | A brief interruption for the lower role |
| P4: same-user takeover (the newest connection of the same user and role replaces the old) | Fixes the crash-and-reconnect case | Anyone holding that user's token can kick that user (they could log in as them anyway) |

## 2.4 Recommended design (Part 2): T1 + P1, P3 and P4 combined

**Threads:**
- **Acceptor thread** (today's `_serverThread`): accept, peer, `AdmitConnection`, **slot admission**, hand the socket to a worker; and `ServiceTokens`/`ReloadTokens` (moved here from `WaitUntilReady`).
- **N pre-auth workers**, default 4 (decision 1), each blocking on its own job queue: `BeginTls` → read **one** Hello frame (64 KiB cap, existing check) → parse it → **verify the token on the worker** (`Authenticate` is const and takes the table pointer under `_tokensMutex` [V]) and the major version → record the rate-limit outcome → offer the connection to the arbiter.
- **Session thread** (new): waits on a single hand-off slot; runs the session; on exit enqueues today's cleanup (`:2156-2164`); then signals "free".
- **Main thread:** unchanged (`ProcessCommands`/`CommandQueue`).

**Hand-off without touching `HandleHello` semantics.** The worker only *filters*. The session thread replays the original Hello payload through the existing `ExecuteCommand(Hello)` on the main thread. That re-authenticates (cheap, constant time), runs the main-thread side effects (`CloseEditGroups`, `SetIdentity`, `_clientErrorCodes`) and builds ServerInfo exactly as today. If the token was revoked between the worker and the replay, the replay refuses and the connection closes as today. A later re-Hello on the session connection keeps its current path.

**Per-connection state:**
- a new `struct Connection { socket; std::unique_ptr<EditorTlsSession>; PeerAddress; connectedAt; deadline; std::atomic<bool> close; }`;
- `Send`/`Receive`/`WaitUntilReady` become `Connection` methods;
- the members `_peer`, `_tlsSession` and `_tlsSessionSocket` go;
- `_identity`, `_peerText`, `_clientErrorCodes` and `_closeConnection` stay as **the session's**, written only at hand-off and session end.

**Budgets and limits:**
- **Pre-auth:**
  - TLS plus one Hello frame within **2 s** from accept for everyone (item 1; the prior-offender budget stays 2 s or drops to 1 s);
  - payload at most 64 KiB (unchanged);
  - no other command is read;
  - pre-auth connections log nothing per connection except outcomes that cross a threshold (item 9 coalescing), because `PostLog` floods the unbounded main queue today.
- **Memory per slot:** one TLS context (about 2 x 16 KiB record buffers plus state [M]) plus at most 64 KiB of payload, so about 150 KiB [M]; N=4 to 8 gives at most about 1.2 MiB.
- **Slot admission** (new pure class `PreAuthSlots`, mutex-guarded):
  - at most `N` in total;
  - at most **2 per non-loopback address**;
  - loopback may use all N (behind a proxy everyone is loopback; see the risks);
  - when full, the new connection is closed at once with no TLS and no log line, and counted (a periodic summary line).
  - Optional (decision 6): reserve 1 slot for non-loopback addresses that authenticated within the last 24 h.
- **Handshake rate** (TLS CPU as a DoS lever): a token bucket in the acceptor, global 20 handshakes/s and per address 5/s [M: an ECDHE-ECDSA P-256 handshake costs about 1-3 ms of server CPU]. Excess is closed before TLS. With N workers, at most N handshakes run at once, so the main thread keeps at least `cores - N` cores. Workers can be lowered in priority (POSIX `nice` per thread is not portable; skip unless measured).
- **Rate limiter (item 8 folded in):**
  - check the token **before** the per-user backoff, so a correct token always gets in and failures are still counted;
  - a timeout, early close or TLS failure counts as a per-address failure (item 1);
  - the loopback global backoff becomes a cap on *pre-auth concurrency and handshake rate for loopback* instead of a total refusal;
  - `RateLimiter` gets one mutex (all its methods are short and logic-only [V]).

**Arbiter** (new pure class `SessionArbiter`, mutex-guarded), given the active session (none or `{user, role}`) and a verified candidate:
- no session: **Accept**;
- same user and role: **Takeover** (P4: close the old one, hand off);
- candidate role strictly higher: **Preempt** (P3);
- otherwise: **Busy** (P1).

**What Busy and Preempt do:**
- **Busy** sends ErrorCoded `busy` if the Hello asked for `errorCodes`, else a plain Error ("The host is serving another session; try again later"), then closes. Whether to name the holder's user or role is decision 5.
- **Takeover and Preempt** set the session's `close` flag. The session thread leaves at its next wait (at most 100 ms, or when the command in flight on the main thread finishes) and runs the normal cleanup, so open edit groups are closed and the autosave is flushed. The candidate waits in its slot for up to 3 s for "free". If the old session does not end in time (a stuck `Send`, until item 2's 30 s write deadline), the candidate gets Busy.
- **No-token host** (loopback only after item 7): every client is admin with an empty user, so the newest connection takes over (P4). Today the second one waits in the backlog (decision 8).

**Interaction with item 2 (S2):**
- The session thread applies the idle read deadline (default 10 min), the write deadline (30 s) and the optional max session age.
- P3 and P4 make the idle timeout a backstop rather than the only remedy.
- Pre-auth workers never use the post-Hello deadlines.
- Add TCP keepalive on the session socket [M: `SO_KEEPALIVE` plus `TCP_KEEPIDLE`/`TCP_KEEPALIVE`, which differ per OS] so half-open sessions are noticed.

**Listen backlog:** `listen(s, N + 4)` (`EditorServer.cpp:1550`). The acceptor drains the backlog quickly, because accept does no handshake.

**Platform:** replace `select` in `WaitUntilReady` with `poll` on POSIX (fixes the `FD_SETSIZE` UB, F18) and `WSAPoll` on Windows [M: WSAPoll's known defect concerns failed outgoing `connect`, which a server does not use]. Each thread still waits on one socket with the 100 ms slice, so `Stop()` keeps its "join within one interval" property (`:1579-1596`): it now joins the acceptor, the N workers and the session thread, and closes sockets only after the joins. On Windows `accept` returns a `SOCKET` that is cast to `int` today; keep it, and note it in the `Connection` type [M].

**Thread-safety plan (what is shared, and how):**
- tokens table: shared_ptr swap under `_tokensMutex` [V];
- `RateLimiter`, `PreAuthSlots`, `SessionArbiter`: one mutex each, with no calls out while holding it;
- hand-off slot: mutex plus condvar;
- session identity, peer text, `_closeConnection`: as today (mutex or atomic), written only by the session thread or at hand-off;
- TLS: shared config read-only after Start, the DRBG locked by Mbed TLS (THREADING_C);
- logging: `PostLog` (thread-safe queue);
- engine state: main thread only (unchanged).

## 2.5 Tests (Part 2)
- **Pure unit tests (no sockets, injected clock):**
  - `PreAuthSlotsTests`: the total cap, the per-address cap, loopback, release, the reserved slot;
  - `SessionArbiterTests`: the role matrix, same-user takeover, the no-token host;
  - the token bucket;
  - `RateLimiterTests` additions: a correct token while the user is backed off gets in; a pre-auth timeout counts.
- **Socket tests** (real loopback, fake peers through `SetPeerAddressProvider`, the injected rate-limit clock, short real pre-auth budgets of 200-300 ms as the existing Hello-timeout tests do [M]):
  - slowloris: N-1 idle sockets from distinct fake addresses, and a legitimate Hello still succeeds within budget;
  - one address opens 10 sockets: 2 are admitted and the rest are closed at once; another address still gets in;
  - an active session's command round trip is unaffected while 100 connect-and-close cycles run;
  - viewer active + admin Hello → the viewer is closed with cleanup enqueued and the admin is served; editor active + editor Hello → Busy (ErrorCoded `busy` when asked); the same user reconnecting takes over;
  - the token revoked between the worker check and the replay → closed, and the next candidate is served;
  - `Stop()` with all slots and the session busy returns within about 1 s, and every thread is joined;
  - N concurrent TLS handshakes succeed (`EditorTlsTests`).
- **CI:** all 8 legs (Windows covers `WSAPoll`). Run the new socket tests under ThreadSanitizer if a sanitizer leg exists [M: not checked]; otherwise propose one Linux Clang TSan job for `editor-server` tests only.

## 2.6 Staged PR plan (Part 2)
Prerequisites: item 1 (pre-auth accounting, 2 s budget) and item 2 (S2 idle and write deadlines) merged.
1. **PR S-1, refactor with no behaviour change:** `Connection` struct; `Send`/`Receive`/`WaitUntilReady` move into it; `poll`/`WSAPoll` replaces `select`; `ServiceTokens` moves to the accept loop. The existing suites stay green.
2. **PR S-2, policy classes only:** `PreAuthSlots`, `SessionArbiter`, token bucket, `RateLimiter` mutex plus the token-before-user-backoff fix (item 8, first half). Unit tests.
3. **PR S-3, the structural change:** acceptor, N workers, session thread, hand-off with Hello replay, the Busy policy (plain Error only), backlog N+4, `Stop` joining all threads, `--preauth-slots` (1-16, default 4). Socket tests.
4. **PR S-4:** preemption (P3) and same-user takeover (P4); ErrorCoded `busy` (new `errorCodes` entry, protocol **1.13.0** minor; clients regenerated, vectors updated); loopback backoff as a concurrency and rate cap (item 8, second half).
5. **PR S-5, docs and deploy:** `docs/editor.html` and `docs/logging-and-editor.html#rate-limit-gaps` (remove "a peer can hold the thread for the Hello timeout"), the slot and rate flags, behaviour behind a proxy.
6. **Frontend PR** after S-4: map `busy` to a clear message with retry-with-backoff, and show "taken over by another session of this user" [M: current frontend reconnect behaviour not checked].

## 2.7 Decisions for the user (Part 2)
1. Slot count: default 4 (recommended), configurable 1-16, or a fixed 8.
2. Second-client policy: P1+P3+P4 (recommended), P1 only, or P2 (queue).
3. Busy signalling: a new `busy` error code with protocol 1.13.0 (recommended), or a plain Error only (no protocol change).
4. Limits: 2 pre-auth slots per non-loopback address, 20/s global and 5/s per-address handshakes (recommended), or other values.
5. Whether the Busy message names the active session's user and role (useful to teams; viewers already see users via PollEvents, F9). Recommended: role only.
6. Reserve a slot for recently authenticated addresses (optional; useless behind a proxy).
7. Threading T1 (recommended) or T2 (event loop: more work, a single core for TLS).
8. No-token host: newest takes over (recommended), or keep waiting in the backlog.

## 2.8 Risks (Part 2)
- **Concurrency bugs.** State that was single-threaded by construction (`_peer`, `_tlsSession`, `_helloTokenRefused`) becomes per-connection. A missed member is a data race. Mitigation: PR S-1 moves the state before any second thread exists, plus TSan.
- **Behind a TLS proxy or tunnel every peer is loopback**, so per-address caps do not apply. A flood of N concurrent loopback connections every 2 s still keeps *new* clients out, though the authenticated session (G1) is safe. The real answer stays the SSH tunnel, or connection limits at the proxy. That is documented and out of scope.
- **TLS CPU.** N workers can use N cores during a flood. This is capped by N and the handshake bucket, but the render or command latency of the main thread can rise. Measure on the Linux leg [M].
- **A behaviour change for clients.** Busy or takeover replace silent waiting in the backlog. The old Electron client shows a generic error until the frontend PR lands.
- **Preemption mid-command.** The lower-role session ends after its in-flight command completes on the main thread. The cleanup (`CloseEditGroups`, `FlushAutosave`) runs before the new Hello replay because both go through the same FIFO `CommandQueue` [V: `CommandQueue.hpp:28-65`]. Keep that ordering in the tests.
- **Stop and join deadlocks** (a worker waiting on "free" while `Stop` waits on the worker). Every wait must also observe `_running` with the 100 ms slice.
- **Platform.** `WSAPoll`, keepalive option names and `accept` socket types differ between Windows and POSIX. CI legs cover Windows and Linux; macOS is untested [M].
