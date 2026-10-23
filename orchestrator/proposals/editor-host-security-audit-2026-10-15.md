# Editor host security audit (2026-10-15)

**Scope:** the N2Engine editor host: `editor-server/`, the `N2EditorHost` binary (`Host.cpp`, `HostOptions.cpp`), the engine paths it reaches (Lua, scene loading, asset import), `deploy/`, and the Electron client's connection layer.

**What was read:**
- N2Engine `origin/master` at `a1aa1005` (the checkout was identical and clean).
- N2Engine-Electron-Frontend `origin/master` at `d002d9f`.

**Method:** read-only. Nothing was built or run, and no files were changed.

**Citations:** line numbers are from those commits. Paths are relative to the N2Engine repository root unless they are marked `[FE]`, meaning the frontend repository.

**Marks used:**
- `[M]` means from memory or general knowledge, not checked in this code.
- `[U]` means it could not be verified by reading.

---

## 1. Executive summary

The authentication and authorisation core is solid. I found no remote memory-corruption issue in the frame parsing. A peer without a token cannot get past the Hello gate.

- **What works:** the tokens are 256-bit random values, kept as SHA-256 hashes and compared in constant time. The token is checked before anything else in Hello. Roles are a default-deny allow list. Shutdown is gated on the network thread. Asset paths are confined by a careful resolver, and the Lua VM has no `io`, `os`, `package` or `debug`.
- **Where the risk is:** almost all of it is **availability**, plus the **intended-but-unbounded power of the editor role**.

The issues that matter for a host on the internet:

1. **One unauthenticated peer can keep everyone else out (High).** The server serves one connection at a time. A peer that connects and sends nothing holds that slot for the 5 s Hello budget, and it is never rate-limited. Reconnecting in a loop keeps legitimate users out indefinitely, and the docs admit this.
2. **Any authenticated session, a viewer's included, can hold the host forever (High).** After Hello there is no idle timeout, no session lifetime and no write deadline. A viewer that stays connected, or stops reading, locks out every admin and editor.
3. **An editor can hang or stop the edit host for good (High).** Lua runs on the edit host in edit mode whenever a scene loads or a loaded script is rewritten. It has no instruction or memory limit, and `Application.Quit` is bound. Pointing the startup scene at the payload makes the hang or quit happen again on every restart.
4. **An editor can crash the host with stack exhaustion (Medium).** Deeply nested `children` in scene JSON recurse with no depth limit.
5. **Fail-open configuration (Medium).** `--bind <non-loopback>` with no token, or with a token but no TLS, starts with only a warning.
6. **Lockout DoS (Medium).** Behind a TLS proxy or tunnel every peer is loopback. Ten failed Hellos a minute from anyone back off all of loopback, for up to 5 minutes and repeatable. The per-user backoff refuses even a correct token, which lets anyone lock out a named user.
7. **Unauthenticated log flooding (Medium).** A connect-and-close loop writes about 3 log lines per connection, with no limit. The `--log-file` has no rotation, and a stalled stderr reader blocks the main thread under the Logger lock.

The Electron client is well hardened: `contextIsolation`, `sandbox`, no HTML sinks, a hard pin-mismatch failure, and TOFU with explicit confirmation. Its main issue is probably an out-of-support Electron major (39) `[M]`.

---

## 2. Threat model

| Attacker | Starting point | What they want | Gates in the way |
|---|---|---|---|
| **A1. Unauthenticated network peer** | Can open TCP to the editor port: directly with `--bind`, through the published Docker port, or as "loopback" behind a TLS proxy | Code execution, reading or writing project files, learning tokens, denial of service | TLS handshake, the Hello deadline (5 s, or 2 s for a prior offender), a 64 KiB cap before Hello, only Hello allowed before auth, rate limiting on failed Hellos only |
| **A2. Authenticated viewer** | Holds a viewer token | Writing, playing, learning other users and paths, denial of service | The Read class only (`RoleAllows`); `--read-only` |
| **A3. Authenticated editor** | Holds an editor token | Escaping the project, reading host files or the TLS key, running code past the sandbox, Shutdown, denial of service | The Write class; asset and scene path resolvers; the Lua sandbox; no Shutdown |
| **A4. Compromised or malicious host, attacking the client** | Controls the server end of a connection the user made | XSS or RCE in the Electron client, stealing the token for another host | Pinning, `contextIsolation` and `sandbox`, text-only rendering |

Out of scope:
- local users who can already write the project or the token files;
- the supply chain of vendored libraries, beyond noting their versions.

---

## 3. Findings

Ranked by severity. Each finding gives evidence, an attack scenario, impact and a fix. Section 7 turns the fixes into PR-sized items.

### Summary table

| # | Sev | Title | Attacker |
|---|---|---|---|
| F1 | **High** | A serial server with idle pre-Hello connections not rate-limited: unauthenticated DoS | A1 |
| F2 | **High** | No idle, session or write timeout after Hello: any role can monopolise the host | A2, A3 (and A1 on a host without a token) |
| F3 | **High** | Editor Lua runs on the edit host in edit mode with no CPU or memory budget, and `Application.Quit` is bound | A3 |
| F4 | Medium | Unbounded recursion in `GameObject::Deserialize`: stack-overflow crash, persistent through the startup scene | A3 |
| F5 | Medium | Fail-open: a non-loopback bind is allowed with no token, or with a plaintext token | config, then A1 |
| F6 | Medium | Global loopback backoff and per-user backoff are unauthenticated lockout levers | A1 |
| F7 | Medium | Unauthenticated log flooding: disk fill, and a stalled stderr blocks the host | A1 |
| F8 | Medium `[M]` | The Electron client is on major 39, probably past end of support | A4 |
| F9 | Low | PollEvents gives viewers the whole process log: users, roles, peer IPs, absolute paths | A2 |
| F10 | Low | Error and success responses carry absolute host paths and raw exception text | A2, A3 |
| F11 | Low | Revoking a token in the file doesn't end the current session until SIGHUP or the next accept | A2, A3 |
| F12 | Low | Log line integrity: backslash not escaped, no `O_NOFOLLOW` on `--log-file`, Unicode format characters kept | A1 to A3 |
| F13 | Low | TLS identity: the key's and folder's permissions are only set at creation, never checked on load | local |
| F14 | Low | Client-side: the loopback regex accepts non-IPs, U+2063 code spoofing, FrameData length not checked | A4 |
| F15 | Low | Untrusted project assets: audio decoders uncapped, textures read up to 2 GiB before checks, `ReadTextFile` uncapped | content |
| F16 | Info | `ResolveScenePath` and `ResolveSceneFile` skip the per-part name checks `ResolveAssetPath` does | A3 |
| F17 | Info | Deploy hardening gaps: no resource caps, no syscall filter, no `ProtectProc`; Docker run flags not given | ops |
| F18 | Info | Small items: `select()` FD_SETSIZE, `TokensMatch` length leak, `SetEntityProperties` reads unbounded JSON, the env token stays in `/proc/<pid>/environ`, IPv6 keying | various |

### F1: High. Serial server plus unrate-limited idle connections

**Evidence**
- `EditorServer.cpp:1550`: `listen(_listenSocket, 1)`, so the backlog is 1.
- `ServerLoop` (`:2052-2172`) accepts and serves one connection at a time, and `HandleClient` runs inline on the only network thread.
- The pre-Hello deadline is `connectedAt + helloBudget` (`:2243-2249`), with a default of 5 s (`EditorServer.hpp:203`).
- Only `_helloTokenRefused` failures are recorded (`:2409-2414`). A timeout only logs (`:2254-2258`, `:2273`), and a peer that closes early is never counted.
- The TLS handshake shares the same budget (`:2115`), and a peer that stalls it is not counted either (`:2206-2215`).
- The docs admit the gap (`docs/editor.html`, "a peer can hold the thread for the Hello timeout on every connection"; `docs/logging-and-editor.html#rate-limit-gaps`).

**Attack:** A1 runs `while true; do nc host 9999 & sleep 4.9; done`, or opens a TCP connection and trickles one byte of a TLS ClientHello. Every legitimate connection waits behind it in the backlog of 1 or is refused. A proxy in front makes it worse (see F6).

**Impact:** complete loss of availability for all users, with no credentials needed.

**Fix:**
- (a) Count a connection that ends without a successful Hello (timeout, early close or handshake failure) as a failure, at least on the per-address table. Keep loopback on its own budget.
- (b) Cut the pre-Hello budget to about 2 s for everyone, so TLS plus Hello must finish quickly.
- (c) Structurally: accept and authenticate on a small bounded pool (for example 8 pending sessions, each with its own deadline), and only *serve* one authenticated session at a time. Pre-auth slots then cannot block the authenticated one.

### F2: High. No idle or session limits after Hello

**Evidence**
- `deadline()` returns `nullopt` once `helloAccepted` is true (`EditorServer.cpp:2244-2249`), so `Receive` waits forever (`:2270-2271`, `:6869-6907`).
- `Send` has no deadline (`:6838-6866`), so a client that stops reading blocks the thread in `WaitUntilReady(socket, true)`.
- A host with no token treats every connection as an admin session (`:2242`).

**Attack:**
- A2, a viewer with a valid token, sends Hello and then idles; or sends `RenderFrame` (1280x720x4 is about 3.7 MB) and never reads, filling the TCP window.
- No admin or editor can connect until the host restarts or a SIGHUP revokes the viewer (F11).
- Shutdown is admin-only, and an admin cannot connect to send it.

**Impact:** the least-privileged role, or anyone on a host without a token, can lock out every other user indefinitely.

**Fix:**
- An idle read timeout after Hello (for example 10 minutes, configurable).
- A write deadline on `Send` (for example 30 s).
- An optional maximum session age.
- Optionally, a higher role preempts: an admin Hello waiting in a pre-auth slot (F1c) ends a viewer's session.

### F3: High. Editor Lua: code execution on the edit host with no resource limits

**Evidence (from a delegated engine read, spot-checked by me)**

*Libraries and sandbox:*
- Only `base`, `math`, `string` and `table` are opened (`engine/src/scripting/LuaRuntime.cpp:45-50`, checked).
- `dofile` and `loadfile` are removed, and `load` only accepts text (`:55-70`, checked).
- `require` goes through ResourceLoader `[U]`.
- It is PUC Lua 5.4.8, not LuaJIT (`external/lua/lua.h`), and there is one global `sol::state` `[U]`.
- There is no `lua_sethook` and no custom allocator (default `l_alloc`, `external/lua/lauxlib.c:1108`) `[U]`.

*How scripts get run:*
- Scene deserialisation in **edit mode** runs each script's top-level chunk. The path is `LuaComponent::Deserialize` → `SetScript` (`engine/src/scripting/LuaComponent.cpp:574-599`, checked), then `LoadScriptInstance` → `RunSource` (`:88, 116, 149`, `[U]`).
- Entry points:
  - `HandleLoadScene` (client-supplied JSON, `EditorServer.cpp:3550-3559`)
  - `OpenSceneFile` (`:6515`)
  - `RestoreSceneSnapshot` (`:5865`)
  - `WritePlaySnapshot` validation (`:4154`)
- Hot reload: `HandleWriteTextAsset` → `NoteAssetWritten(force=true)` → `ReloadLoadedAsset` → `LuaRuntime::ReloadModule` (`EditorServer.cpp:5745`, `:5309-5321`).

*Exposed binding:* `Application.Quit` is bound (`engine/src/scripting/bindings/LuaApplicationBindings.cpp:13`, checked), and the host loop exits on `IsQuitRequested` (`Host.cpp:538`).

*Persistence:* `SetStartupScene` (`EditorServer.cpp:6788-6831`) makes the scene open on every start (`Host.cpp:360-372`).

*Classification:* the docs class this as an intended editor power ("Write: … runs project code", `EditorServer.hpp:528`), and the Lua sandbox is documented (`docs/scripting-lua.html#sandbox`).

**Attack:** A3 runs
1. `WriteTextAsset res://scripts/x.lua "while true do end"` (or `local t={} while true do t[#t+1]=string.rep('x',1e8) end`);
2. `CreateEntityEx` plus `AddComponent LuaComponent{scriptPath=res://scripts/x.lua}`, or `LoadScene` with that component;
3. `SaveSceneToFile` and `SetStartupScene`.

The main thread spins or runs out of memory at once. It does so again on every restart, and `Restart=on-failure` doesn't help a hang. `Application.Quit()` instead stops the host cleanly, and systemd does *not* restart it, because the exit code is 0.

**What an editor can and cannot do today:**
- **Can:** write any text asset under `assets/`; run sandboxed Lua on the host whenever a scene loads; burn unbounded CPU and memory; quit the host; change global Lua state that other scripts share.
- **Cannot:** reach the file system or processes from Lua (no `io`, `os`, `package` or `debug`); read the tokens file or the TLS key through Lua; Shutdown through the protocol.

**Fix:**
- Install an instruction-count hook (`lua_sethook` with `LUA_MASKCOUNT`) with a per-call budget, and a `lua_newstate` allocator with a byte cap. Raise a Lua error and log it when either is exceeded.
- On the edit host, do not run script chunks at deserialisation in edit mode: load fields from a static parse, or run the chunk in a throwaway state under the budget.
- Make `Application.Quit` a no-op, or a logged refusal, on an editor host.
- Make running project Lua on a remote host a separate right, for example a `scripts` capability or `--allow-editor-lua`, so a pure-content editor role is possible.
- Document plainly that "editor" means "can run sandboxed code on the host".

### F4: Medium. Stack overflow from nested scene `children`

**Evidence**
- `GameObject::Deserialize` recurses on `children` with no depth limit (`engine/src/gameobject/GameObject.cpp:1002-1010`, checked).
- `HandleLoadScene` parses up to 64 MiB of client JSON with no `CheckJsonDepth` (`EditorServer.cpp:3550`). Compare `ReadBoundedJson` (`Commands.hpp:135-156`), which is used only for some command fields.
- `WriteTextAsset` accepts `.scene` up to 4 MiB (`EditorServer.cpp:206-208`, `:5713`), and `OpenSceneFile` and `WritePlaySnapshot` parse that with no depth check.
- `Scene::Serialize` and `dump` are recursive too `[M]`.

**Attack:** A3 sends `LoadScene` with `{"gameObjects":[{"name":"a","children":[{"name":"a","children":[...]}]}]}`, nested about 100,000 deep (about 3 MB). The host takes SIGSEGV. Saving that scene and making it the startup scene produces a crash loop.

**Impact:** an editor can crash the host, and keep it crashing.

**Fix:**
- Run `CheckJsonDepth` (for example a maximum of 128) on every scene and prefab text, whether it comes from the client or from disk.
- Add a hard depth parameter to `GameObject::Deserialize`, and refuse past about 256, matching the glTF node depth cap.
- Test strategy: a unit test with a 10,000-deep scene that expects an Error, not a crash.

### F5: Medium. Fail-open bind

**Evidence**
- `Host.cpp:478-500` only *warns* when `--bind` is not loopback and there is no token (":… anything that can reach it can control this host"), or a plaintext token.
- `EditorServer.cpp:1521` accepts any IPv4 address, `0.0.0.0` included.
- Help text: `HostOptions.cpp:341-344`.
- The Docker `CMD` binds `0.0.0.0` with `--token-env`, so it fails closed when the variable is unset (`deploy/Dockerfile:236-247`). Good, but an operator who overrides `CMD` and drops `--token-env` gets an open host.

**Attack:** an operator runs `N2EditorHost --bind 0.0.0.0 --project …`. A1 connects, sends no Hello (not needed), and has the admin role: `DeleteScene`, `WriteTextAsset`, Lua (F3), `Shutdown`.

**Fix:**
- Refuse to start with a non-loopback bind unless a token *and* `--tls` are present, or `--insecure-bind` is given explicitly.
- Optionally refuse `--play` with a non-loopback bind, since a play host is always plaintext (`HostOptions.cpp:374-375`).

### F6: Medium. Lockout levers in the rate limiter

**Evidence**
- All of loopback shares one counter: 10 failures in 60 s start a global backoff, doubling up to 5 minutes (`RateLimiter.cpp:248-273`, `RateLimiter.hpp:80-82`). A success does not clear it (`RateLimiter.hpp:28-29`).
- The per-user check refuses a Hello naming a backed-off user *before the token is checked* (`EditorServer.cpp:2361-2370`). Any well-formed name counts (`UserNamedByHello`, `:132-148`). User names are visible to viewers through PollEvents (F9).
- The docs and the Dockerfile recommend "an SSH tunnel or a TLS proxy in front" (`deploy/Dockerfile:236-237`). Behind a TLS proxy, every internet peer is loopback.

**Attack:**
- Behind a TLS proxy, A1 sends 10 Hellos with wrong tokens a minute. Every user is closed at accept for 1 s, 2 s, … 5 minutes, repeatable for ever. That is effectively F1 with fewer packets.
- Separately, A1 with a few addresses names `alice` in wrong Hellos. Alice's correct token is refused for up to 30 s at a time, repeatable.

**Fix:**
- Support a trusted-proxy mode (PROXY protocol v2 from a configured upstream address), so per-address limits work behind a proxy.
- Check the token *before* applying the per-user backoff, so a correct token always gets in (keep counting failures).
- Make the loopback backoff a cap on the rate of failures rather than a total lockout: delay the response, or limit concurrent pre-auth loopback connections, instead of refusing all of loopback.
- Document that the SSH tunnel is the safe option, because only SSH-authenticated users reach loopback.

### F7: Medium. Unauthenticated log flooding and stderr back-pressure

**Evidence**
- Every accepted connection logs "Waiting for editor connection...", "Editor connected peer=…" and "Editor disconnected peer=…" (`EditorServer.cpp:2056`, `:2135`, `:2153`). A TLS failure adds a line (`:2201`).
- None of these is rate-limited unless the peer is already backed off, and a connect-and-close never is.
- `--log-file` has no rotation (`HostOptions.cpp:390-392`).
- `StderrLogOutput` does a blocking `fwrite` plus `fflush` (`LogSink.cpp:138-146`) inside the Logger dispatch, which holds the Logger's lock (`LogSink.cpp:229-233`). Every command runs on the main thread, and the network thread waits for its future (`EditorServer.cpp:2379-2390`).

**Attack:** A1 opens and closes TCP connections as fast as possible. That is about 300 bytes of log per connection, gigabytes a day, filling `/var/lib` or the journal. The audit lines are buried, and the EventRing evicts real events (F9). If stderr is a pipe whose reader stalls (journald under pressure, or `docker logs` with a blocking driver), the main thread blocks in `fflush` and the host freezes.

**Fix:**
- Coalesce connection lines: log "N connections without Hello from <peer> in the last minute" at most once per minute per peer, and keep per-connection lines only for sessions that authenticated.
- Make stderr writes non-blocking or bounded: queue to a logging thread and drop with a counter when it is full.
- Add size-based rotation, or say in the docs that `logrotate` is required.

### F8: Medium `[M]`. Electron 39 is probably out of support

**Evidence:** `[FE] package.json` pins `"electron": "^39.2.7"`, and `[FE] package-lock.json:1362-1364` resolves it to 39.2.7. Electron supports the three newest majors on an 8-week cadence `[M]`, so 39 (about October 2025) is likely unsupported by October 2026 `[U]`.

**Impact:** a malicious host (A4) only reaches the renderer as escaped text, so exposure is limited. But any future renderer bug would sit on an unpatched Chromium.

**Fix:** upgrade to a supported major, and add Electron fuses when packaging lands.

### F9: Low. PollEvents gives viewers the whole log

**Evidence**
- The EventRing subscribes to every Logger line from construction, *before* `Application::Init` (`Host.cpp:280-283`; `EditorServer.hpp:88-91`).
- PollEvents is Read class (`EditorServer.cpp:2578`).

What a viewer can read, including the backlog from before they connected:
- audit lines with every user, role, command and `peer=` IP (`:2338-2339`, `:2793-2794`);
- refused Hellos with claimed users and IPs (`:3029-3030`);
- rate-limit lines (`:1930-1948`);
- the tokens-file path (`Host.cpp:446`);
- absolute project and snapshot paths (`EditorServer.cpp:4183`);
- Lua errors with source excerpts.

**Impact:** information disclosure to the least-privileged role: who works when, from which IP, and the host layout. No secrets: tokens and hashes are never logged (verified in `HandleHello` at `:3009-3034`, `LoggableClaimedUser` at `:1911-1923`, and `TokenTable` errors at `TokenTable.cpp:136-140`).

**Fix:** tag log events with a sensitivity level, and filter `audit:`, `Rate limit:` and peer-bearing lines out of PollEvents for non-admins. Alternatively, give viewers only `sceneChanged`, `assetsChanged` and the other non-log kinds.

### F10: Low. Host paths and raw exception text in responses

**Evidence**
- `ExecuteCommand` returns `"Command failed: " + e.what()` (`EditorServer.cpp:2469-2472`). A `std::filesystem_error`'s text contains absolute paths `[M]`.
- `ReadTextFile` returns `"can't read " + Utf8(file)` with the absolute path (`:181-195`), and `HandleReadTextAsset` passes that to the client (`:5678`).
- `WritePlaySnapshot` returns the absolute snapshot path (`:4183-4185`), which is by design for a local launcher.
- `CreateScene` and `GetCurrentScene` echo `e.what()` (`:3536`, `:3696`).

**Fix:** map engine exceptions to generic messages for remote clients and keep the detail in the server log (admin-only per F9). Return the snapshot path as `res://`-relative, or `.n2/play/<name>`, when the client is not local.

### F11: Low. Revoking in the file doesn't end the live session

**Evidence**
- `ReloadTokens(false)`, the signature check, runs only at accept (`EditorServer.cpp:2132`).
- During a session the network thread only reloads on the SIGHUP flag (`ServiceTokens`, `:1823-1829`, called from `WaitUntilReady`, `:6918`).
- Combined with F2's unlimited session, a revoked user stays connected until an operator sends SIGHUP. The tokens unit documents `systemctl reload` (`deploy/n2editorhost-tokens.service:108-114`), but the Docker path relies on `docker kill -s HUP`.
- The signature is size plus mtime (`TokenTable.cpp:222-236`). Replacing one 64-hex hash with another keeps the size, so detection depends on mtime granularity.

**Fix:** in `WaitUntilReady`, also run the cheap signature check every few seconds. Use a content hash as the signature, or add the inode and ctime.

### F12: Low. Log line integrity

**Evidence**
- `EscapeLogText` escapes control characters but not `\` itself (`LogSink.cpp:102-131`). Client text containing a literal `\n` cannot be told apart from an escaped newline by a tool that unescapes the line.
- Unicode bidi and line separators (U+202E, U+2028) pass through. Most client text is also `SanitizeForLog`'d, which only replaces bytes below 0x20 and 0x7F (`EditorServer.cpp:1994-2017`).
- `--log-file` is opened without `O_NOFOLLOW` (`LogSink.cpp:167`), so a symlink planted in a writable log folder redirects appends.
- Existing files keep their mode and only get a warning (`:182-194`).

**Fix:** escape `\` as `\\`. Escape U+2028, U+2029 and U+202A-U+202E (and U+2066-U+2069) as `\u{…}`. Open with `O_NOFOLLOW` and refuse a file the host's user doesn't own.

### F13: Low. TLS identity file permissions

**Evidence**
- The folder is set to 0700 only when it is created (`EditorTls.cpp:546-557`).
- A key file is created through `ofstream`, then `chmod 0600` before it is written (`:272-280`). That is fine, but the default umask applies until the `chmod`.
- On load, neither the key's nor the folder's mode is checked (`:585-611`).

Otherwise the TLS setup is sound:
- TLS 1.2 minimum, `PRESET_DEFAULT` (`:619-625`), Mbed TLS 3.6.7 (`external/mbedtls/include/mbedtls/build_info.h:37`);
- ECDSA P-256, a random 128-bit serial, `CN=N2EditorHost`, validity 2024-2074, with dates intentionally ignored under pinning (`:351-433`);
- no session-ticket callback, so no server-side resumption state;
- renegotiation is not enabled; it is off by default in Mbed TLS 3.x `[M]`, and `Send` treats WantRead as failure (`EditorServer.cpp:6851`);
- the fingerprint is the SHA-256 of the DER certificate (`:635-641`).

**Fix:** on POSIX, refuse to load a key whose mode allows group or others, or that is not owned by the effective uid, as the tokens file does (`TokenTable.cpp:274-276`).

### F14: Low. Client-side parsing gaps (A4)

All three are `[FE]` findings from a delegated read.

- **Loopback check.** `src/protocol/fingerprint.ts:25-31` uses `/^127(?:\.\d{1,3}){3}$/`, so `127.999.0.1` passes the check for "plaintext allowed" and goes to `getaddrinfo` as a hostname.
  *Fix:* `net.isIP` plus an octet range check, or resolve the name and require every address to be loopback.
- **U+2063 code spoofing.** A host's plain `Error("⁣readOnly⁣…")` is parsed as coded (`src/shared/api.ts:612-620`). There is no privilege gain, since a host can send `ErrorCoded` anyway.
  *Fix:* strip U+2063 from uncoded messages, or carry the code in a separate IPC field.
- **FrameData length.** `decodeFrameDataResponse` doesn't check `pixels.length == width*height*4` (`editor-server/clients/web/protocol.generated.ts:1813-1818`).
  *Fix:* validate in the generator.

What the client does well: TOFU requires explicit confirmation; a mismatch hard-fails before any token is sent; there is no override path; the token is never persisted or logged; `contextIsolation`, `sandbox` and `nodeIntegration:false` are set; navigation and window opening are denied; the IPC sender is checked; the CSP is `script-src 'self'`; there are no `innerHTML` or `eval` sinks; and ssh is spawned without a shell, with `--` and validated arguments.

### F15: Low. Untrusted project content (assets that came from git or a shared folder)

There is no protocol command that imports a binary asset: no ImportAsset command exists. These parsers therefore only meet files an operator or git put in `assets/`. Details from a delegated read, `[U]` unless marked:

- **Textures:** `Texture.cpp:32-51` reads up to 2 GiB into memory before the stb dimension checks. `STBI_MAX_DIMENSIONS` is 16384, and the 256 MiB decode cap is fine.
- **Audio:** dr_wav allocates `totalPCMFrameCount*channels` from the header with no cap (`engine/src/audio/AudioLoaders.cpp:39-40`), and ogg, mp3 and flac decode the whole file.
- **Text files:** `ReadTextFile` has no size cap (`EditorServer.cpp:181-195`, checked). It is used for scene, snapshot and autosave files.
- **glTF and FBX (good):** a 512 MiB file cap, a 256 MiB parse-memory allocator, the engine's own buffer loader with caps, `ResolveModelUri` confining external URIs (no `..`, absolute, `:` or device names, checked after symlinks), `cgltf_validate`, and a node depth of 256. ufbx has 1 GiB allocator limits and `load_external_files=false`.
- **Versions:** cgltf 1.15, ufbx 0.23.1, stb_image 2.30, stb_vorbis 1.22, dr_wav 0.14.3, dr_mp3 0.7.3, dr_flac 0.13.2, nlohmann 3.12.0 (`external/nlohmann/include/nlohmann/json.hpp:68`, checked). Check these against current CVE lists `[U]`. stb_image and stb_vorbis have a long fuzzing-bug history `[M]`.

**Fix:** a file-size cap before read in `Texture.cpp` (for example 256 MiB); a cap on audio frame count times channels; a size cap on `ReadTextFile` matching `MaxTextAssetBytes` times some factor.

### F16: Info. Scene path resolvers lack the per-part name checks

**Evidence**
- `ResolveAssetPath` validates every path part with `IsValidAssetName`, which rejects `:` (NTFS streams), device names, trailing dots and spaces (`EditorServer.cpp:5242-5258`, `:5113-5143`).
- `ResolveScenePath` (`:6344-6408`) and `ResolveSceneFile` (DeleteScene, `:2515-2557`) only reject absolute paths and paths with a root.

**Attack:** on Windows, `res://ab:c.scene` names the alternate data stream `c.scene` of a file `ab`. Writes stay inside the assets root (the lexical and canonical checks still hold), so this is a confusion issue, not an escape.

**Fix:** have `ResolveScenePath` call the same part validation, and the same for `ResolveSceneFile`.

### F17: Info. Deploy hardening

**Evidence:** both units (`deploy/n2editorhost.service:49-68`, `deploy/n2editorhost-tokens.service:149-168`) already have `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `PrivateDevices`, the `ProtectKernel*` set, `RestrictAddressFamilies`, `RestrictNamespaces`, `CapabilityBoundingSet=` (empty), `UMask=0077`, `LockPersonality`, `RestrictSUIDSGID` and `SystemCallArchitectures`. That is good.

Missing from the units:
- `MemoryMax=` and `TasksMax=`, which would contain F3, F7 and F15;
- `SystemCallFilter=@system-service` plus `~@privileged @resources`;
- `ProtectProc=invisible` and `ProcSubset=pid`;
- `IPAddressDeny=any` with `IPAddressAllow=localhost`, since the host binds loopback here;
- `MemoryDenyWriteExecute=yes`, which is safe because the interpreter is PUC Lua, not a JIT; check the software renderer and Mesa first `[U]`;
- `PrivateUsers=yes`;
- `LimitNOFILE=`.

`AF_UNIX` is probably not needed `[U]`.

Docker (`deploy/Dockerfile`):
- It runs as a non-root user (uid 10001), and has no secrets in its layers. Good.
- The docs do not give run flags: `--cap-drop ALL --security-opt no-new-privileges --read-only --memory … --pids-limit …`.
- The token passed through `-e` is visible in `docker inspect`. Prefer `--tokens-file` mounted as a secret with mode 0400.

The env-var token is `unsetenv`'d (`HostOptions.cpp:441-446`), but `/proc/<pid>/environ` still holds the original block `[M]`. Only same-uid processes can read it, and Lua has no `io`, so this is Info.

**Docs vs reality:** the claims checked (loopback default, a 64 KiB cap before Hello, only Hello before auth, a 0600 tokens file, a 0600 log file on create, TLS 1.2+, idle connections not backed off) match the code. The single-token unit's "the host never sees the file" is true; it sees the variable.

### F18: Info. Small items

- **`select()` with `FD_SET`.** `EditorServer.cpp:6930-6936` is undefined on POSIX for fd ≥ 1024. The host keeps few fds, but a large asset scan or Lua would not open many either. Consider `poll()`.
- **`TokensMatch` length leak.** It reveals a length mismatch early (`:2030-2040`), which is documented. Generated tokens have a fixed length, so this is negligible.
- **`SetEntityPropertiesCmd`.** It uses unbounded `ReadJson` (`Commands.hpp:612`), unlike the other JSON fields (`ReadBoundedJson`). Only known keys are read (`ParseEntityProperties`), and nlohmann 3.12 destroys values iteratively `[M]`, so this is consistency only.
- **IPv6.** The listener is IPv4-only (`EditorServer.cpp:1518-1525`). If IPv6 is added, keying the per-address table on /128 (`RateLimiter.cpp:213`, `:276`) is trivially bypassed; key on /64.
- **Viewer-class commands that change session state.** SetViewportSize, SetEditorCamera and GetAudio, which drains the stream buffer, are all classified Read (`:2574-2582`). This is harmless with one client at a time, but must be revisited if sessions ever become concurrent.
- **`Step`.** It runs up to 1000 game frames synchronously per command (`EditorServer.hpp:474`), so an editor can stall a play host. Play is local-only (remote play is disabled per `docs/logging-and-editor.html`).

---

## 4. What I checked and found sound

### Frame parsing
- The header is `[u8 type][u32 len]`. The length is checked against `MaxPayloadBytesBeforeHello` (64 KiB, applied while gated and before Hello) or `MaxPayloadBytes` (64 MiB) **before** the allocation `std::vector<uint8_t> payload(payloadLength)` (`EditorServer.cpp:2277-2294`).
- An oversize frame closes the connection, because it cannot be resynchronised.
- Partial reads loop until complete, under the deadline (`:6869-6907`).
- A zero-length payload is fine: handlers throw `out_of_range` on short reads, which becomes an Error.
- Unknown ids are refused for non-admins by default-deny before any handler, and get "Unknown command" in the default branch (`:2995-3000`).
- **There is no integer overflow:** `BufferReader::Require` compares `size > Remaining()` without adding to `_pos` (`Serialization.hpp:110-117`), and `ReadString` copies exactly `len ≤ remaining`.
- No handler `reserve`s by a client-supplied count. Arrays in requests are JSON with caps (`MaxBoundsEntityIds` 4096, `MaxInputEvents` 1024, `ReadBoundedJson` depth 64 and 200,000 values, `Commands.hpp:97-156`).
- Optional trailing fields are read only when bytes remain (`ReadTrailing`, `Commands.hpp:162-168`; `HelloCmd`, `:201-208`).

### Generated codecs
- The C++ (`clients/cpp/Protocol.generated.hpp`) and C# (`clients/csharp/Protocol.generated.cs`) outputs are declarations only; they contain no decoders.
- The TypeScript `ProtocolReader` bounds-checks every read (`protocol.generated.ts:906-977`), and `decodeFrame` checks the declared length against the available bytes (`:1011-1020`).
- Lists in responses are JSON. The only decoder-level gap is F14's FrameData size.

### Pre-auth surface
- With a token, only Hello is queued before a successful Hello. Anything else gets a fixed Error and is closed (`EditorServer.cpp:2303-2310`).
- Shutdown before Hello is blocked by the same gate.
- The Hello payload is parsed on the network thread only to extract the user for the rate limiter (`:132-148`).

### Hello order and what a refused client learns
- The token is checked first (`:3020-3036`), so a client without it learns only "Invalid access token". There is no version, engine version, capabilities, project or role.
- A wrong user and a wrong token give the same message.
- The rate-limited reply is given for any well-formed name, existing or not, so there is no user-existence oracle (`:2361-2370`, `:132-148`).
- `ErrorCoded` is only enabled after a successful Hello (`:3078`).

### Token comparison
- `TokenHashesEqual` reads all 32 bytes (`TokenTable.cpp:112-119`).
- `TokenTable::Authenticate` evaluates every entry without an early exit and selects by mask (`:202-218`).
- `TokensMatch` is constant time apart from the length (`EditorServer.cpp:2030-2040`).
- Tokens are 256 bits from the Mbed TLS CTR-DRBG (`TokenTable.cpp:317-338`, `EditorTls.cpp:451-473`).

### Tokens file
- On POSIX: one `open`, then `fstat` on the descriptor, `S_ISREG`, a group/other mode check, a size cap, and a read from the same descriptor. There is no TOCTOU between check and read (`TokenTable.cpp:259-297`).
- It follows symlinks and checks the target, which is documented.
- Parse errors never echo file content or hashes (`:136-140`, `:174-176`).
- Duplicate users and duplicate hashes are refused.
- A reload is swapped under a mutex. A failed reload keeps the old table and is not retried until the file changes (`EditorServer.cpp:1771-1802`).
- A user removed or whose role changed is disconnected on reload (`:1804-1821`).
- SIGHUP only sets a flag, which is signal-safe (`Host.cpp:40-54`).
- `--token-env` is removed from the environment before the engine and Lua start (`HostOptions.cpp:441-446`).

### Authorisation
- `RoleAllows` is admin=all, editor=not Admin, viewer=Read only, with unclassified ids denied (`EditorServer.cpp:1831-1848`).
- It is applied in `ProcessCommand` before dispatch (`:2770-2777`), and for Shutdown on the network thread (`:2324-2340`).
- The `--read-only` gate is also default-deny (`:2763-2767`).
- The play commands (`WritePlaySnapshot`, `SetPaused`, `Step`, `SendInput`) are Write, so a viewer cannot play (`:2637-2640`).
- Read-class handlers (RenderFrame, PickEntity, GetEntityBounds, ReadTextAsset, ListAssets, GetLuaFields, GetComponent) only read. **GetLuaFields does not run Lua:** it uses the already-loaded component's `DescribeFields`, with no load (`:5053-5079`).

### Asset paths
`ResolveAssetPath` (`:5216-5297`) checks, in order:
1. it starts with `res://` and has no NUL;
2. every part passes `IsValidAssetName` (no `..`, `:`, `\`, control characters, device names, or a trailing dot or space);
3. the path is lexically inside the root;
4. the parent is inside the root after `weakly_canonical`;
5. an existing target is inside the root after canonicalisation, so symlinks cannot escape.

Further checks:
- ReadTextAsset and WriteTextAsset use an extension allow list and a 4 MiB cap, and require UTF-8 without NUL. Writes are atomic, by temp file and rename `[U]` (`IO::WriteTextFileAtomically`), so a hardlink is replaced rather than written through.
- The open scene's file cannot be overwritten (`:5692-5751`).
- CreateFolder and CreateScriptAsset go through the same resolver. CreateScriptAsset only writes a fixed template.
- ListAssets skips symlinks while walking (`:5452-5463`).
- `.meta` is not a text-asset extension, so it cannot be rewritten through WriteTextAsset.
- DeleteScene accepts only `.json` under the scenes folder, canonicalises the parent, and removes a symlink as the link itself (`:2515-2557`).
- `scriptPath` values in scenes must be indexed assets before they load `[U]`.

### Lua
- No `io`, `os`, `package`, `debug`, `dofile` or `loadfile`; no binary chunks (`LuaRuntime.cpp:45-77`).
- No process spawning anywhere in engine or host code `[U]` (delegated grep).

### TLS
See F13. The handshake runs inside the Hello budget, a failure closes the connection, and the identity lives outside the project (`Host.cpp:289-303`).

### Logging
- Tokens and hashes never reach a log.
- The claimed user is logged only if it exists in the current table (`LoggableClaimedUser`, `:1911-1923`).
- Control characters are escaped in every output (`LogSink.cpp:102-131`).
- The log file is created 0600 (`:167`).
- `--log-file` failing to open is fatal at start.

### Deploy defaults
- The default bind is 127.0.0.1 (`EditorServer.hpp:98`, `HostOptions.cpp:341`).
- Both units bind loopback and use TLS.
- Docker fails closed without the token variable.

---

## 5. Could not verify

- How the Electron client's direct-remote and SSH flows and the host behave under real network conditions. Nothing was run, and no fuzzing was done.
- That `IO::WriteTextFileAtomically` uses a unique temp name in the same folder, and `rename`/`ReplaceFile` that never follows a symlink at the destination. I read only its call sites.
- That `LuaComponent` → `RunSource` has no edit-mode guard (the engine chain at `LuaComponent.cpp:88-149`), and that the `require` implementation (`LuaRuntime.cpp:160-179`) blocks `..`. Both come from a delegated read.
- Whether every sol2 userdata binding is memory-safe against hostile Lua (for example a dangling GameObject after Destroy).
- CVE status of the vendored cgltf, ufbx, stb, dr_libs, FreeType and Mbed TLS 3.6.7.
- Mbed TLS's actual default cipher list and renegotiation state in this build, `[M]` for 3.6 defaults: TLS 1.3 enabled; renegotiation compiled in but disabled at runtime.
- Whether `Scene::Serialize`, autosave and undo snapshots recurse as deeply as `Deserialize` does (affects F4's reach).
- The Electron support window (F8).
- Whether nlohmann 3.12's `dump()` recursion could be reached with a deep value the host keeps. The command-side JSON is depth-capped, and scene JSON is F4.

---

## 6. Recommended fix items (PR-sized)

| # | Item | Fixes | Size | Suggested tests |
|---|---|---|---|---|
| 1 | **Pre-auth accounting.** Count a connection that ends without a successful Hello (timeout, close or TLS failure) as a per-address failure; separate weight for loopback; pre-Hello budget 2 s | F1 | S | `RateLimitServerTests`: a fake clock plus a peer provider; N idle connections from one address are then refused at accept; a loopback peer that idles does not lock out the next |
| 2 | **Session timeouts.** An idle read deadline after Hello (`--idle-timeout`, default 10 min), a write deadline in `Send` (30 s), and an optional `--max-session` | F2 | S-M | `EditorServerHardeningTests`: authenticate, idle past the timeout, expect close; a client that never reads a RenderFrame is closed after the write deadline (small socket buffers) |
| 3 | **Concurrent pre-auth slots.** Accept and run TLS and Hello for up to N pending connections in parallel; serve one authenticated session; optionally let a higher role preempt | F1, F2, F6 | M-L | A slowloris test: 8 idle sockets do not stop a 9th client's Hello |
| 4 | **Lua budgets.** `lua_sethook` instruction count per entry point, and a capped `lua_Alloc`; raising either is a Lua error | F3 | M | Engine tests: `while true do end` in a module returns an error within the budget; `string.rep` over the cap fails; the host stays responsive |
| 5 | **No script execution in edit-mode deserialisation**, or a throwaway budgeted state; `Application.Quit` refused on an editor host | F3 | M | `EditorPlayTests` and `EditorAssetTests`: LoadScene with a script that sets a global or calls Quit leaves no global and the host running |
| 6 | **Scene depth limit.** `CheckJsonDepth` on all scene, prefab and snapshot text (client or disk); depth cap in `GameObject::Deserialize` | F4 | S | Unit: a 10,000-deep `children` chain gives an Error, not SIGSEGV, for LoadScene, OpenScene and WritePlaySnapshot |
| 7 | **Fail closed on bind.** A non-loopback `--bind` requires a token and `--tls` unless `--insecure-bind`; `--play` plus a non-loopback bind is refused | F5 | S | `HostOptionsTests` and `HostTests`: argument combinations give exit code 1 and a message |
| 8 | **Rate limiter.** Check the token before the user backoff; loopback backoff as a rate cap, not a total lockout; optional PROXY v2 from `--trusted-proxy` | F6 | M | `RateLimiterTests`: a backed-off user with the right token gets in; a loopback burst delays rather than refuses all |
| 9 | **Log volume.** Coalesce unauthenticated connection lines per peer per minute; stderr through a bounded async queue with a drop counter | F7 | M | `HostLogSinkTests`: 1000 connect-and-close cycles give at most a few lines; a blocked stderr (a pipe nobody reads) does not stall `ProcessCommands` |
| 10 | **Viewer event filtering.** Sensitivity tags on log lines; non-admins get no `audit:`, `Rate limit:` or peer lines, and no host absolute paths | F9 | S-M | `PollEventsTests` per role |
| 11 | **Sanitised errors.** Generic messages for filesystem and engine exceptions to the client, detail in the log; `res://`-relative paths in responses | F10 | S | Golden-message tests for ReadTextAsset and WritePlaySnapshot failures |
| 12 | **Live revocation.** Periodic signature check (or inotify) during a session; a content hash in the signature | F11 | S | `EditorTokensTests`: rewrite the file with the same size and mtime but a different hash, and the session closes within about 5 s without SIGHUP |
| 13 | **Log hardening.** Escape `\`, bidi and line-separator characters; `O_NOFOLLOW`; owner check | F12 | S | `HostLogSinkTests`: escaping round-trip; a symlink log path is refused |
| 14 | **TLS identity permission check on load** (POSIX: owner and 0600 key, 0700 folder) | F13 | S | `EditorTlsTests`: a 0644 key is refused |
| 15 | **Scene resolver part validation**, shared with `ResolveAssetPath`, for `ResolveScenePath` and `ResolveSceneFile` | F16 | S | Unit: `res://ab:c.scene`, `res://con.scene` and `res://x./y.scene` are refused |
| 16 | **Asset size caps.** Texture read cap, audio frame-count cap, `ReadTextFile` cap | F15 | S | Engine tests with crafted headers (a huge WAV `dataSize`, a 1 GiB sparse PNG) |
| 17 | **Deploy.** `MemoryMax`, `TasksMax`, `SystemCallFilter`, `ProtectProc`, `ProcSubset`, `IPAddressAllow=localhost`, `MemoryDenyWriteExecute` (verify), Docker run flags in the docs | F17 | S | `systemd-analyze security` score in CI or docs; a smoke start under the unit in the Linux CI leg if one exists |
| 18 | **Frontend.** Electron upgrade; strict loopback IP check; strip U+2063 from uncoded errors; FrameData size check in the generator | F8, F14 | S each | Frontend unit tests (`fingerprint.test`, `api.test`); a generator vector for a short FrameData |
| 19 | **Small hygiene.** `poll()` instead of `select()`; `ReadBoundedJson` for SetEntityProperties | F18 | S | Existing hardening tests |

**Suggested order:**
1. Items 1, 2, 6 and 7. These are small and close the cheapest DoS paths and the misconfiguration.
2. Items 4 and 5, the editor's code-execution budget.
3. Items 8 and 9.
4. Item 3, the structural fix.
5. The rest.
