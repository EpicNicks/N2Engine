Netcode stage A6: `N2Server`, the headless dedicated server, and the join-token generator A5 deferred to it.

## What it is

- **`N2Server`** (`n2server/`, library `N2ServerCore`): a new executable, native builds only. It loads a project and a scene, runs the fixed-tick loop with no window, no renderer and no audio, and listens on a `SecureTransport` over `UdpTransport` (the A5 handshake). It authenticates clients, enforces `--max-clients` and the pre-auth slots and rate limits already in n2security, and logs connects and disconnects. No replication or gameplay networking (track B): authenticated clients' datagrams are counted and dropped.
- **Arguments:** `--project`, `--scene`, `--bind`, `-p/--port`, `--tokens-file`, `--max-clients`, `--protocol-id`, `--insecure-bind`, `--relay` (parsed, refused as not implemented, exit 1 before anything starts), `--exit-on-stdin-eof`, `--log-level`, `--log-file`, `--new-token`, `--help`.
- **Fail closed:** a non-loopback `--bind` is refused unless `--tokens-file` or `--insecure-bind` is given; the default bind is 127.0.0.1 and nothing else changes it. The tokens file goes through `JoinTokenTable::Load` (regular file, 1 MiB, private on POSIX, well formed) before the engine starts.
- **Ready line** on stdout once listening: `N2Server ready port=<port> bind=<address>`.
- **Stopping:** SIGINT/SIGTERM, Windows console Ctrl+C/Break/close/logoff/shutdown (the handler waits up to 4.5 s for the clean shutdown), `Application::Quit`, stdin EOF with the flag; clients get a sealed Disconnect. SIGHUP (POSIX, with a tokens file) re-reads the tokens file; a bad file keeps the old table.
- **`N2Server --new-token <user> <role> --tokens-file <path>`:** 256 random bits, hex; adds `{"user","role","hash"}` (SHA-256) to the file atomically (new file 0600 on POSIX, existing file checked like the server checks it, duplicate user refused, symlink followed), prints the token once on a `token: ` line. Nothing logs or writes the secret.

## Engine changes (small, defaults unchanged)

`ApplicationOptions` gains `RenderBackend::NONE` (no GLFW, no window, no renderer; `Window::IsValid()` false, `IsWindowless()` true, `ShouldClose()` false), `audio` (false opens no device at all) and `physicsGpu` (false forces the physics GPU off, also for the Deterministic-preset checks). `Resources::LoadShader` returns null without a renderer. Every existing host keeps the old behaviour.

## Tests

- `n2server_tests` (`tests/n2server/`): argument parsing and the token CLI as plain logic.
- `n2server_smoke_client` plus `tests/smoke/n2server_smoke.py` (ctest `n2server_smoke`): starts the real N2Server on loopback with a tokens file made by `--new-token`, waits for the ready line, joins as player and spectator through the A5 handshake, checks a wrong token and a wrong protocol id never connect, checks the connect/disconnect log lines and that no token reaches any output, and stops it (stdin EOF everywhere; SIGTERM, SIGINT and a SIGHUP reload on POSIX). Also checks `--relay` and the bind rule are refused.
- `WindowlessWindowTest.NoRendererBackendCreatesNeitherAWindowNorARenderer`.
- The three Linux build steps now also build `N2Server`, `n2server_tests` and `n2server_smoke_client`; Windows builds everything and runs ctest.

## Docs

`docs/netcode.html` (new section "The dedicated server: N2Server": flags, start order, ready line, tokens, security, running and stopping, service/Docker hints, two design decisions, a security-review callout), `docs/application.html` (the three new options, "No renderer at all"), `docs/testing.html`, `docs/index.html`.

## Judgement calls (not in the plan)

- `--protocol-id` (default `0x4E324E54`, the id the netcode docs use) and default port 7777.
- Unknown arguments are errors, unlike N2EditorHost, which ignores them.
- N2Server links `N2EditorServer` for its `LogSink`, parsers, stdin watcher and `EditorServer::ResolveScenePath` instead of copying them.
- SIGHUP reload of the tokens file (the plan did not list it; without it a token added by `--new-token` needs a restart).
- The project's resource loader is not opened read-only, so it may write asset metadata into the project folder.

## Not verified

- Nothing was built or run locally: CI is the first compile of every file in this PR.
- The systemd unit and Docker notes in the docs were not run; neither was the Windows console-close/logoff handler.
- Real load, a real network, a NAT or a firewall; a project whose scene uses a renderer in its components, headless (the smoke scene is empty).
- Security review of the fail-closed rule, the tokens-file handling and the generator's atomic write is still to do (the docs say so).

## Approved deviations
Accepted by the user 2026-10-13:
- Added --protocol-id (default 0x4E324E54, the id the netcode docs use) and default port 7777: the plan lists neither, the secure layer needs a protocol id.
- Unknown arguments are errors, unlike N2EditorHost which ignores them (a typo must not leave a server running with the wrong settings).
- N2Server links N2EditorServer (read-only reuse of LogSink, ParsePort/ParseWholeNumber, StartStdinEofWatcher, EditorServer::ResolveScenePath) instead of duplicating them.
- Added SIGHUP reload of the tokens file (POSIX, only with --tokens-file); not in the plan, but --new-token appends to a file a running server would otherwise never re-read.
- Engine: ApplicationOptions gained RenderBackend::NONE, audio and physicsGpu (defaults unchanged); Window::ShouldClose is false for a windowless window without a renderer; Resources::LoadShader returns null with no renderer.
- Fail-closed rule: non-loopback bind needs --tokens-file or --insecure-bind; a loopback server without a tokens file starts but admits nobody (warned), since every session is encrypted whatever the flags.
- Resource loader is not opened read-only on the server, so it may write asset metadata into the project folder.
- Smoke test is a Python script plus a C++ test client (Python has no X25519/ChaCha20); registered as ctest n2server_smoke, and the three Linux build steps now name N2Server, n2server_tests and n2server_smoke_client.
- No dedicated Docker or systemd files added (deploy/ is outside the allowed paths); the docs carry an inline unit and Docker notes, marked not verified.


## Review

An independent review of 02070df found no open blocker or high findings; CI is green on all jobs. Remaining notes: n2server/src/Server.cpp:673 The std::exception catch returns 1 without OnApplicationQuit or app.Shutdown(), so scenes, physics and audio are torn down only by static destruction, which Application::Shutdown's own comment says is unsafe.; docs/netcode.html:606 'Nothing else is written to stdout' is false in Debug builds: Application::Init calls Logger::InitializeDebugConsoleHelper under N2ENGINE_DEBUG, which echoes every Logger line to stdout, including the Admin-tagged 'Client connected: user=... address=...' lines.; docs/netcode.html:627 The Security section says the proofs are matched with BasicTokenTable::Authenticate, but SecureTransport::OnConnectResponse runs its own constant-time HMAC loop over Entries() and never calls Authenticate.; n2server/src/Server.cpp:555 When PeerConnected has no identity, the warning says the session 'is dropped', but nothing disconnects it (SecureTransport::Disconnect is a no-op inside Poll's handler and is not called later); it is only left out of the clients map..

