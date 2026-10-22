# Task editor-tls-frontend: Frontend half of native TLS: fingerprint pinning and a direct-address remote
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-editor-tls-frontend.

## Issue / plan
Engine PR #112 is merged (master c0eee5e); see engine docs/logging-and-editor.html#ready-line. Engine side prints `N2EditorHost ready port=<port> tls=1 fingerprint=<64 lowercase hex, no colons>` when started with --tls, and serves TLS 1.2+/1.3 with a self-signed certificate. Frontend: (1) parseReadyLine in src/main/host-launcher.ts returns {port, fingerprint?}; unknown key=value fields stay ignored, a line without tls=1 behaves as today. (2) Node tls pinning with no new dependency, behind the SocketFactory seam in src/protocol/engine-client.ts (open() ~L172) and connectTo in src/main/engine-ipc.ts: tls.connect({host, port, rejectUnauthorized:false, checkServerIdentity:()=>undefined}), then on secureConnect compare socket.getPeerCertificate().fingerprint256 normalised to lowercase hex without colons to the pinned value and destroy the socket on mismatch; the Hello/token handshake happens only after the pin check passes. (3) A trust-on-first-use pin store, like src/main/recent-remotes.ts: key host:port, value fingerprint plus first-seen time; first connect to an unknown host asks the user in the connect form to confirm the displayed fingerprint (never auto-trust silently), a changed fingerprint is refused with a clear message that names the old and new values and how to reset (a Forget pin button); the store is a JSON file in userData. (4) A direct-address remote option beside the SSH tunnel in the remote form (src/main/remote-ipc.ts, RemoteSettings): host, port, token, and the expected fingerprint (optional, pins up front); TLS is required for non-loopback direct addresses (refuse plaintext to a non-loopback host). The SSH tunnel mode is unchanged. Play mode stays disabled for remote engines. (5) Tests with node:test for parseReadyLine, the fingerprint normaliser, the pin store (first use, match, mismatch, forget) and the pin check against a local tls server with a generated self-signed cert (do NOT commit a PEM or any BEGIN ... PRIVATE KEY text, it trips the ratify secret scan: generate a throwaway self-signed key and cert once with the openssl CLI at development time, commit them as base64 DER strings in a test helper clearly marked test-only, and rebuild the PEM at runtime with node:crypto createPrivateKey/X509Certificate export; also test that a different cert is refused). (6) README: a Remote editing section for the direct-address mode, the trust-on-first-use model, what it does not give (no per-user tokens or rate limiting; see engine docs), and that it is untested against a real cloud host. Run npm run typecheck, build and test.

## Allowed paths (regex, repo-relative)
- ^src/
- ^README.md$

## Acceptance
- npm run typecheck, npm run build and npm test pass locally
- Tests as listed; the fixture key is clearly test-only
- README documents the direct-address mode and its limits

## Notes
Security-sensitive: the reviewer looks hard at the order (pin check before sending the token), normalisation of fingerprints, never auto-trusting, and not logging the token. No PEM literals in the repo (ratify secret scan); DER base64 fixture rebuilt to PEM at runtime. Not verifiable here: a real cloud host. List under unverified.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs editor-tls-frontend` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/editor-tls-frontend/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-editor-tls-frontend --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/editor-tls-frontend/dev.json: {"pr":<number>,"branch":"feat-editor-tls-frontend","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-editor-tls-frontend` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
