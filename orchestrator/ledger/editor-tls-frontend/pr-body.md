Frontend half of native TLS (engine PR #112): the editor can connect straight to an `N2EditorHost --tls` at its own address, trusting its self-signed certificate by SHA-256 fingerprint.

## What changed

- `parseReadyLine` returns `{port, fingerprint?}`. The fingerprint is read only from a `tls=1` line; unknown fields are still ignored and a line without `tls=1` behaves as before. A `tls=1` line with a missing or malformed fingerprint is refused as malformed.
- `src/protocol/tls-pin.ts` (no new dependency): `tls.connect` with `rejectUnauthorized: false` and `checkServerIdentity: () => undefined`, min TLS 1.2, a handshake timeout. On `secureConnect` the peer's `fingerprint256` is normalised (lowercase hex, no colons) and compared to the pin; on mismatch the socket is destroyed with a `FingerprintMismatchError`. `onConnect` runs only after the pin check, so `EngineClient` sends Hello (and the token) only to a host that passed. It sits behind a second factory next to `SocketFactory`; `ConnectOptions.tlsFingerprint` selects it.
- `EngineClient.connect` refuses plaintext to any host that is not loopback, before opening a socket. `EngineHost.connectTo` takes an optional `{host, tlsFingerprint}` target.
- Trust on first use: `PinStore` (`pinned-fingerprints.json` in userData, `host:port` -> fingerprint + first-seen time, re-validated on read, atomic write). A pin is never replaced silently.
- Direct-address remote: `ProjectSession.connectDirect`, `remote:connectDirect` / `remote:inspectDirect` / `remote:forgetPin` IPC, and a "Direct address (TLS)" tab in the remote form beside the SSH tunnel (SSH mode unchanged). The form does a handshake-only inspect (nothing sent), shows the fingerprint for an unknown host and connects only on an explicit confirm, refuses a changed fingerprint naming old and new values with a **Forget pin** button, and takes an optional expected fingerprint to pin up front. Play stays disabled for remote engines.
- Tests (`node:test`): `parseReadyLine`, the normaliser, the pin store (first use, match, mismatch, forget, bad entries), the pin check against a local TLS server (match, different cert refused with no bytes received, plain server, timeout), `EngineClient` end to end (token never reaches a mismatching host), `parseDirectRemoteSettings`, and `connectDirect`. The throwaway test certificates are base64 DER strings in `src/test/test-tls-identity.ts` (marked test-only, rebuilt to PEM at runtime); no PEM text in the repo.
- README: "Remote editing: direct address (TLS)".

## Review focus

- Order: pin check before Hello/token (`connectPinnedTls`, `EngineClient.open`/`connect`).
- Fingerprint normalisation (`src/protocol/fingerprint.ts`): exact 64 hex, or 32 colon-separated pairs; nothing partial is accepted.
- Never auto-trusting: `requiredFingerprint` throws when there is neither a typed/confirmed fingerprint nor a pin; the token is not logged or put in any message or state.

## Not verified

- Against a real `N2EditorHost --tls`, and against a real cloud host: only a local Node TLS server with a throwaway certificate.
- The new form (UI) was typechecked and bundled but not run in Electron here.


## Approved deviations
Accepted by the user 2026-10-10:
- Direct mode has an explicit TLS checkbox (default on); plaintext is allowed only for loopback hosts, enforced in the form, parseDirectRemoteSettings and EngineClient.connect
- TOFU flow: a handshake-only remote:inspectDirect (sends nothing) shows the fingerprint; the confirmed value is passed as settings.fingerprint and the pin is stored only after a successful Hello
- Added remote:inspectDirect and remote:forgetPin IPC channels and a pure src/protocol/fingerprint.ts shared with the renderer bundle
- A tls=1 ready line with a missing or malformed fingerprint makes parseReadyLine return null (treated as not a ready line)
- Direct remotes are not added to Recent Remotes (only pins are remembered)
- Added a handshake timeout (10 s) to the pinned TLS connect and the inspect
- New non-test source files use CRLF to match siblings; new test files LF
- Forget pin is now always shown beside Connect in the direct form when TLS is on (not only after an inspect), and the error texts say use Forget pin in the connection form
- HostState gained optional direct flag so toolbar, console and stop/restart refusals drop the SSH tunnel wording for direct connects
