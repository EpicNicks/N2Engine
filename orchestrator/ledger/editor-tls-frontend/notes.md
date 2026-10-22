# Notes (editor-tls-frontend, 2026-10-10)
Approved deviations (accepted by the user):
- Direct mode has an explicit TLS checkbox (default on); plaintext is allowed only for loopback hosts, enforced in the form, parseDirectRemoteSettings and EngineClient.connect
- TOFU flow: a handshake-only remote:inspectDirect (sends nothing) shows the fingerprint; the confirmed value is passed as settings.fingerprint and the pin is stored only after a successful Hello
- Added remote:inspectDirect and remote:forgetPin IPC channels and a pure src/protocol/fingerprint.ts shared with the renderer bundle
- A tls=1 ready line with a missing or malformed fingerprint makes parseReadyLine return null (treated as not a ready line)
- Direct remotes are not added to Recent Remotes (only pins are remembered)
- Added a handshake timeout (10 s) to the pinned TLS connect and the inspect
- New non-test source files use CRLF to match siblings; new test files LF
- Forget pin is now always shown beside Connect in the direct form when TLS is on (not only after an inspect), and the error texts say use Forget pin in the connection form
- HostState gained optional direct flag so toolbar, console and stop/restart refusals drop the SSH tunnel wording for direct connects
Unverified: a real N2EditorHost --tls and a real cloud host; the form was not run in Electron.
Deferred: per-user tokens, rate limiting, read-only mode.
