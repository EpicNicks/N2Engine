# Exposed editor host hardening: per-user tokens, rate limiting, read-only mode (proposal, 2026-10-13)
Tags: [V] verified in the repo at master 20c18ab, [M] assumption. Paths relative to N2Engine/.

## 1. What exists today
- [V] Single accept loop: editor-server/src/EditorServer.cpp:1690 ServerLoop accepts, BeginTls (:1716), then HandleClient (:1734) to completion. ONE client at a time; a second connection waits in the listen backlog. The peer address is never read (accept(listenSocket, nullptr, nullptr), :1698), so logs have no source IP.
- [V] Auth: one token, `_accessToken` (SetAccessToken :1583), from --token-env (HostOptions.cpp:123; ReadAccessToken :301 removes the variable from the environment). Check in HandleHello :2337 with constant-time TokensMatch :1672 (only the length leaks). Token is checked before the version, so a wrong token learns nothing.
- [V] Gate: with a token only Hello runs before a good Hello (IsAllowedBeforeHello :1684, HandleClient :1883); a refused Hello closes the connection (:1935); Hello deadline 5000 ms from accept, covering the TLS handshake (EditorServer.hpp:173); pre-Hello payload cap 64 KiB (:100), later cap 64 MiB (:97). Tests: tests/editor-server/EditorServerHardeningTests.cpp:842-967.
- [V] No lockout, backoff, per-address state or concurrent-connection concept. An unauthenticated client costs the host up to 5 s of its only serving thread per connection, so it can starve the legitimate editor. This DoS is a bigger practical risk than guessing a 256-bit token.
- [V] Identity in logs: only the self-declared clientName (SanitizeForLog :1636) in "Refused Hello" (:2339) and "said Hello" (:2373). No user, no address, and nothing says who issued a later edit. ServerInfo = protocolVersion, engineVersion, capabilities (always empty, :1666), projectLoaded (protocol/protocol.json:61-77). Errors are plain strings; there are no error codes.
- [V] Command classification: `IsEditOnlyCommand` (EditorServer.cpp:3357) is applied only when `_playMode` (:2114). It lists 13 commands that write project files or switch scenes (OpenScene, NewScene, SaveSceneToFile, DeleteScene, SetProjectSettings, SetStartupScene, Restore/DiscardAutosave, LoadScene, SetImportSettings, WriteTextAsset, CreateScriptAsset, CreateFolder). It is NOT a read/write table: in-memory mutation (CreateEntity, DestroyEntity, SetEntityProperties, AddComponent, SetComponentFields, Undo/Redo, SaveScene, WritePlaySnapshot, SendInput, RescanAssets, CreateScript, Shutdown) is absent because a play host plays a throwaway snapshot. Reusing it unchanged for read-only would leave the editor writable.
- [V] Hashing with no new deps: with N2ENGINE_EDITOR_TLS ON, Mbed TLS is linked (editor-server/CMakeLists.txt:42-59) and EditorTls.cpp:44,607 already calls mbedtls_sha256. With TLS OFF there is no SHA-256 in editor-server. No SIGHUP handler anywhere; Host.cpp:53-66 handles SIGINT/SIGTERM only.
- [V] Docs defer all three (orchestrator/ledger/editor-tls/notes.md, "Deferred hardening"). The systemd unit takes the token from an env file (deploy/n2editorhost.service:6-8).

## 2. Design and recommendation

### 2a. Per-user tokens
- Format: `--tokens-file <path>`, JSON, one entry per user: `{"user":"alice","role":"editor","hash":"sha256:<64 hex>"}`. Never plaintext. Tokens are 256-bit random, made by a new `N2EditorHost --new-token <user> <role>` that prints the token once plus the line to add. Machine-generated tokens make unsalted SHA-256 adequate (nothing to brute-force). Compare the hash of the presented token to ALL entries with the existing constant-time loop, no early exit.
- Hash: SHA-256 via Mbed TLS, so the tokens file requires an N2ENGINE_EDITOR_TLS build (the exposed build anyway; [M] confirm the Dockerfile and Linux CI leg turn it on). Fallback: vendor a small public-domain SHA-256 (about 150 lines, FIPS test vectors) so it works with TLS OFF and runs in every CI leg.
- Roles: `admin` (everything), `editor` (everything except Shutdown and the project-level destructive commands, list to be agreed), `viewer` (read-only, 2c). Role is bound to the connection at Hello.
- `--token-env` stays and means one synthetic admin entry (user name "token"). Recommend it be mutually exclusive with --tokens-file (startup error), for clarity.
- Revocation without restart: re-stat the file (mtime+size) at each accepted connection and on SIGHUP (POSIX only; mtime is the portable path). A failed reload keeps the old table and logs a Warn. A reload that removes the user of the open connection closes it (flag checked in the HandleClient loop, [M] small change). Swap the table only on the network thread (single-threaded [V]).
- File safety: refuse to start if group/world readable on POSIX; reject entries whose hash was not made by --new-token (reject short or non-hex). Suggested location /etc/n2editorhost/tokens.json (root-only, like the existing env file) or a Docker secret mount; user decides.
- Identity: log `user=alice role=editor peer=<addr>` on every Hello success/failure; add an `audit:` Info line per non-polled mutating command carrying the user (polled commands stay silent, IsPolledCommand :1971). ServerInfo gains trailing `user`, `role`, `readOnly`.

### 2b. Rate limiting and lockout
- getpeername after accept; per-address map {failures, nextAllowed} on the network thread, capped (about 4096, evict oldest) so it is not itself a memory DoS. IPv4 only today (--bind takes an IPv4 address, HostOptions.cpp:259).
- Policy: after 3 failed Hellos from an address, backoff 1 s doubling to 5 min; reset on success or 15 min idle. A banned address is accepted and closed immediately, before the TLS handshake (the costly part). Prior offenders get a shorter 2 s TLS+Hello budget.
- Residual risk: because serving is serial, backoff reduces but cannot remove starvation. The real fix is a separate pre-auth accept thread that hands only authenticated connections to the session; recommend DEFERRING it (bigger ServerLoop refactor). A firewall allow-list stays the primary control.
- Reverse proxy / SSH tunnel (the documented path, deploy unit comment line 14): every peer is 127.0.0.1, so a per-address ban would lock out everyone. Recommendation: do NOT support X-Forwarded-For (no such header in this binary protocol; PROXY protocol would be the only equivalent and spoofing it defeats the lockout). Rule: loopback peers are exempt from per-address bans but subject to one global failed-Hello backoff (e.g. 10 failures/minute across all loopback). Better with per-user tokens: key backoff on the user named in Hello.

### 2c. Read-only mode
- Two switches, one gate: role `viewer` (per user) and `--read-only` (whole host, also with the legacy token or no token). The gate sits beside the _playMode check (:2114).
- Do not reuse IsEditOnlyCommand as is (section 1). Add `IsReadCommand`, an ALLOW list, default deny. Seed from IsPolledCommand (:1971, mostly reads) plus GetCurrentScene, GetOpenScene, GetProjectInfo, GetPlayState. Add a test that iterates every command id in protocol.json and requires an explicit class (read / write / admin), so a new command cannot ship unclassified.
- Judgement calls: RenderFrame, SetViewportSize, SetEditorCamera, PickEntity affect only the view: allow. SendInput and WritePlaySnapshot (run project Lua on the host): deny for viewers. PollEvents may expose host log lines [M: check content for paths].
- Denial: today SendError(string) (:2116). Prefer a trailing `code` on Error if the format can take one [M, not checked]; else a stable `[read-only]` message prefix, documented.
- Frontend (EpicNicks/N2Engine-Electron-Frontend, not examined [M]): regenerate from protocol.json (clients/web/protocol.generated.ts exists [V]); read role/readOnly from ServerInfo; disable create/delete, inspector edits, script save, import settings, Save/Undo/Redo, input-driven Play; show a "read-only" badge; map the denial to a toast; add an optional user field to the connect dialog.

## 3. Protocol impact (protocol.json 1.10.0 to 1.11.0, minor)
- Hello request: trailing optional `user: string` (absent from older clients; then the token is matched across all entries). ServerInfo response: trailing `user`, `role`, `readOnly`. Both are trailing fields, so a minor bump under the rule at protocol.json:21. Optional Error `code`. Regenerate clients/cpp, csharp, web and update protocol/test-vectors.json and protocol/tests/typescript-codecs.test.mjs [V exist]. Old client against new host works (denials are ordinary errors).

## 4. Staged plan (each PR must be green on Windows and Linux CI; the tests are socket/unit tests that run on every leg)
1. PR A, classification: IsReadCommand, exhaustive-classification test, `--read-only`, denial error. Useful alone, no auth change.
2. PR B, per-user tokens: store, --tokens-file, --new-token, roles in the gate, audit fields, protocol 1.11.0 and regenerated clients. Tests: hash vectors; revoke by rewriting the file between two connections (portable); perms refusal (POSIX only, skipped on Windows); --token-env still admin. SHA-256 tests run only on legs with N2ENGINE_EDITOR_TLS unless SHA-256 is vendored [M: check .github workflow].
3. PR C, rate limiting: peer address, backoff map with an injected clock (tests never sleep), close-fast banned peers, loopback global rule, tests with a fake address.
4. PR D, docs and deploy: docs/editor.html, docs/logging-and-editor.html, deploy unit (`ExecReload=/bin/kill -HUP $MAINPID`), Dockerfile secret mount; remove the "deferred" notes.
5. Frontend PR, separate repo, after B (can start after A for the flag).
Order: A, then B and C, then D, with the frontend after B.

## 5. Risks
- Viewer with SendInput/WritePlaySnapshot means remote Lua execution: keep denied.
- Unsalted SHA-256 is safe only for machine-generated tokens; enforce via --new-token.
- Reload races and open-session revocation need care; keep all state on the network thread.
- Serial server: lockout can lock out the real user if an attacker shares their address (tunnel case).
- Classification drift: guarded by the exhaustive test.

## 6. User decisions
1. Roles: is admin/editor/viewer enough; which commands are admin-only; may viewers play. 2. Tokens file location, exclusive with --token-env or not. 3. Require the TLS build for SHA-256, or vendor it. 4. Reverse proxy: support nothing (recommended) vs PROXY protocol; loopback policy. 5. Accept the serial-server starvation risk, or fund the pre-auth accept thread. 6. Error code vs message prefix.

## 7. Cannot be verified without a real exposed host
Real source addresses (NAT, load balancer, tunnel), lockout behaviour under real latency, SIGHUP through systemd and `docker kill -s HUP`, file permission semantics for Docker secrets and Windows ACLs, the 5 s Hello budget over WAN with TLS, and the frontend UI.

## Decisions (user, 2026-10-13)
- Roles admin / editor / viewer; viewers are strictly read-only and may NOT enter play mode (play needs editor).
- Per-user token hashing uses Mbed TLS SHA-256: the tokens file requires a TLS build (--token-env stays for single-token and plain setups, exclusive with the file).
- No trusted-proxy support (no PROXY protocol / X-Forwarded-For); loopback gets one global backoff, not per-address bans. The pre-auth accept thread (5 s single-client starvation window) is deferred and documented.
- Protocol 1.11.0 (minor): trailing Hello `user`, trailing ServerInfo `user`/`role`/`readOnly`, and a new denial error code; clients regenerate; frontend PR after.
- Sequencing: PR A read-only classification (default-deny IsReadCommand allow list + a test forcing every protocol.json command to be classified), PR B per-user tokens, PR C rate limiting, PR D docs/deploy, then the frontend PR. Start after #121/#122 and the Windows slim items have merged (EditorServer.cpp and protocol.json are shared).
