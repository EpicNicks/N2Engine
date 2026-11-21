# Issue #149: live-host verification checklist (Electron editor and N2EditorHost)

For a human, on the GPU PC and (for the remote parts) a second machine you can ssh to. Everything about the host (flags, the ready
line, error codes, roles, log lines) was checked against the code and docs on master (308f4f8b). Everything about the Electron
frontend (button labels, where the badge sits, how the ssh tunnel is started) lives in the separate frontend repo, which this
checklist did not read: those parts come from `orchestrator/proposals/frontend-dom-tests.md`, `verification-notes-2026-10-14.md`
and `project-memory/pending-minor-defects.md`, and are marked **[frontend, unchecked]**. Where I could not verify something it says
so. The hardware list is in `user-verification-checklist.md` ("Needs a real remote host" and "Needs the Electron editor run against
a live host").

Report for each step: pass, fail or not tried; the editor and host build (commit), OS, GPU and driver; and any text in the editor's
error banner or the host log. File a defect per failure.

## 0. Setup

Host build, Windows (GPU PC) or Linux. TLS and tokens-file need `-DN2ENGINE_EDITOR_TLS=ON` (a host without it exits 1 on `--tls` and
on `--tokens-file`; `docs/logging-and-editor.html`, `--tls` row). The Linux recipe is in `docs/editor.html` ("Running the host on
Linux"): `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DN2ENGINE_EDITOR_TLS=ON ...` then
`cmake --build build --parallel --target N2EditorHost`, binary at `build/bin/N2EditorHost`.

Make a project (exit 0 created, 2 already exists, 1 other error; prints one `N2EditorHost created projectId=<uuid> startupScene=...`
line, `HostOptions.cpp` help text):

```
N2EditorHost --create /path/to/proj --name "Live Check"
```

Sanity check the host alone, before the editor, with the repo's smoke script (standard-library Python 3,
`tests/smoke/editor_host_smoke.py`; scenarios `host`, `tls`, `attach`, `mcp`, `all`):

```
python tests/smoke/editor_host_smoke.py --exe build/bin/N2EditorHost --scenario host --expect-physics any
python tests/smoke/editor_host_smoke.py --exe build/bin/N2EditorHost --scenario tls
```

Expected: `Smoke test passed (host)` / `Smoke test passed (tls)`, exit 0. Use `--config Debug` for a Debug build (Debug logs to the
console). `--scenario host` runs the software renderer, so it does not exercise the GPU: step 1 does.

## 1. Electron editor against a live host on the GPU PC (item 1)

Start a host with the OpenGL renderer (the default; `--renderer software` is the other choice) and a token:

```
set N2_EDITOR_TOKEN=<random>      (PowerShell: $env:N2_EDITOR_TOKEN = "<random>")
N2EditorHost --project C:\path\to\proj --port 9999 --token-env N2_EDITOR_TOKEN
```

The host reads the variable and removes it from its own environment, so Lua cannot see it. Expected stdout, once listening (the line
is the launcher's contract, `docs/logging-and-editor.html` "The ready line"):

```
N2EditorHost ready port=9999
```

On a machine with a GPU the host log (stderr, `<UTC time> INFO ...`; stdout too in a Debug build) should say OpenGL is used, not
`Using Software renderer, headless`. If the OpenGL window cannot be created the host falls back to serving with no window
(`docs/testing.html`, second host of the smoke test): note whether `GetEngineHealth` (the editor's health panel) says Renderer Running.

Then in the editor (local mode, token field filled, or the editor's own launched host if it has one) check, and note anything odd:

- [ ] Connect succeeds; the viewport shows the startup scene lit on the GPU (frames arrive at a steady rate; no black or flipped frame).
- [ ] Hierarchy, inspector, add and remove a component, edit a field, undo and redo, save (unsaved marker clears).
- [ ] Editor camera and picking in the viewport; gizmo drag; a cancelled drag (Esc) leaves nothing dirty.
- [ ] Asset browser: add a file outside the editor, it appears within about a second (the host polls, `docs/editor.html`).
- [ ] Lua script edit hot-reloads.
- [ ] **Play (OpenGL play child, E9).** Press Play. The launcher asks the edit host for `WritePlaySnapshot`, then starts a second host with
  `--play <snapshot> --project <same> --renderer <same> --exit-on-disconnect` (`docs/editor.html` play section; the HostOptions help for
  `--play`). Expected: toolbar goes Starting, Playing; the play child renders on the GPU; Pause, Step and input work; the inspector is
  read-only while playing; Stop returns to Edit and the child process exits (check Task Manager or `ps` for an orphaned N2EditorHost).
  Look for: black frames, a hidden window popping up, the child not exiting, edit-mode commands accepted while playing.
- [ ] Audio from the play child (streamed to the client through `GetAudio`; loopback device).
- [ ] Shutdown: closing the editor stops the host it launched (`--exit-on-stdin-eof` / `--exit-on-disconnect`).

## 2. Remote mode over a real ssh server (item 2, part 1)

On the remote machine: a host bound to loopback (the default; never pass `--bind` for this), TLS on, a token:

```
N2_EDITOR_TOKEN=<random> N2EditorHost --project /srv/proj --port 9999 --renderer software --tls --token-env N2_EDITOR_TOKEN
```

Expected ready line: `N2EditorHost ready port=9999 tls=1 fingerprint=<64 lowercase hex digits>`. Copy the fingerprint: it is
the SHA-256 of the DER certificate, no colons (`docs/logging-and-editor.html`).

First confirm what the editor will wait for, by hand, from the editor PC. The frontend starts a built-in
`ssh -L 127.0.0.1:0:...` tunnel (port 0 so the local OS picks the port) and waits for ssh to print
`Allocated port N for local forward` **[frontend, unchecked; the exact ssh arguments are in the frontend repo]**. The line is from
reading the OpenSSH source and was never observed on a real server. To see what a real ssh prints, run the same shape yourself:

```
ssh -N -L 127.0.0.1:0:127.0.0.1:9999 user@remote
```

- [ ] Does ssh print `Allocated port <N> for local forward.` (or similar) on stderr, with and without `-v`? Record the exact text,
  the OpenSSH client version (`ssh -V`), and the client OS (Windows OpenSSH differs from Linux). If it does not print at all without
  `-v`, the frontend's readiness signal must change (this is the finding the notes expect might happen).
- [ ] In the editor: Remote engine mode, enter the ssh target and the token. Expected: connects, project opens, viewport frames arrive.
  Record how long connect takes and whether a wrong host key / password prompt is handled (the editor cannot answer an interactive
  prompt; key-based login is the assumption).
- [ ] Wrong ssh target or refused tunnel: the editor shows an error, not a hang.
- [ ] Play mode is disabled in remote mode (by design; notes: "Play mode disabled remotely"). Confirm the button is disabled and says why.
- [ ] Tunnel dies mid-session (kill the ssh process): the editor shows a disconnected state, not a frozen viewport.
- [ ] Check the host log on the remote: peer is `127.0.0.1` behind the tunnel (expected, `docs/editor.html` "Rate limiting and the
  tunnel": no per-address ban, no shared loopback backoff).

## 3. Direct-address TLS pinning (item 2, part 2)

Needs a host on a non-loopback address. Host side (a non-loopback `--bind` is refused at start unless a token and `--tls` are both
given, exit 1 before the engine starts, naming what is missing; `docs/editor.html` Binding):

```
N2_EDITOR_TOKEN=<random> N2EditorHost --project /srv/proj --port 9999 --bind 0.0.0.0 --renderer software --tls --token-env N2_EDITOR_TOKEN
```

Open only that port to your editor PC's address (firewall). Then, in the editor's direct-remote form **[frontend, unchecked; labels from
`frontend-dom-tests.md` and the frontend notes]**:

- [ ] Enter host, port, token with TLS checked, press connect. Expected: a prompt that says the host "isn't trusted yet" and shows the
  host's fingerprint. Compare it character by character with the `fingerprint=` on the host's ready line (or
  `journalctl -u n2editorhost | grep fingerprint`). `connectDirect` must not have run: check the host log has no `said Hello` line yet.
- [ ] Press "Trust this fingerprint and connect". Expected: connects; the host log shows `Editor client '...' said Hello ...`; the pin is
  saved only after a successful Hello (never auto-trusted).
- [ ] Inspect (shows the fingerprint without connecting) matches.
- [ ] Reconnect: no prompt this time (pinned), connects.
- [ ] **Forget pin**: press it, reconnect: the first-connect prompt comes back.
- [ ] **Changed fingerprint is refused.** Stop the host, delete its identity (`$XDG_DATA_HOME/n2engine/editor-tls/`, else
  `$HOME/.local/share/n2engine/editor-tls/`; on Windows the equivalent user data folder, **not checked** which folder) and start it again so it
  makes a new self-signed certificate (the ready line shows a new fingerprint). Connect from the editor with the old pin kept.
  Expected: refused with a message that the fingerprint changed, no token sent, no way to proceed except Forget pin.
- [ ] Unchecking TLS for a non-loopback host shows "Plaintext is only allowed to this computer" and does not connect.
- [ ] Handshake timeout: point it at a port where something listens but does not speak TLS; expected a clear error within a few
  seconds (the host side allows 5 s for a handshake, then 2 s for the Hello).
- [ ] Throughput over the WAN (viewport frame rate, large scene load) is acceptable; note the numbers (RTT and frame rate).
- [ ] Wrong token three times: the host backs the address off (1 s doubling to 5 min; `Rate limit: ...` in the host log) and the editor
  shows a message, then the right token works again after the backoff.
- [ ] Windows only: the TLS key file and a tokens file permissions. The host's 0600 check is POSIX-only, so on Windows ACLs are never
  checked (`user-verification-checklist.md`): look at the file's ACL and note who can read it.

## 4. Tokens-file host, read-only role, and the error code through contextBridge (item 3)

Make tokens (printed once; `--new-token` needs the TLS build; the file holds only the SHA-256):

```
N2EditorHost --new-token alice admin
N2EditorHost --new-token bob editor
N2EditorHost --new-token carol viewer
```

Put the three printed entries in a JSON array, for example `[{"user":"alice","role":"admin","hash":"sha256:..."}, ...]`, save it as
`tokens.json` and, on POSIX, `chmod 600 tokens.json` (the host refuses a file readable by group or others: "accessible to its group or to
others", exit 1 at start). Start the host (do not combine `--tokens-file` with `--token-env`):

```
N2EditorHost --project /srv/proj --port 9999 --renderer software --tls --tokens-file /path/tokens.json
```

- [ ] Connect as carol (viewer): the editor shows a user/role badge (carol, viewer) **[frontend, unchecked where it sits]**. Every write
  control and Play are disabled; reading, the viewport and the camera work.
- [ ] Force a refused command anyway (for example a stale button, or a second editor build that does not disable it, or the smoke
  client). The host answers `ErrorCoded` with code `forbidden` and the text `forbidden: role viewer is not allowed to run <command>`
  (`docs/logging-and-editor.html`, "A refusal is ErrorCoded..."); the connection stays open. The host log has
  `audit: denied user=carol role=viewer command=...`.
- [ ] **The error code survives Electron's contextBridge.** The frontend carries the code across the bridge as a U+2063-delimited message
  prefix and strips it with `errorCodeOf`/`errorTextOf` (notes, frontend #28); this was only simulated in tests. Check in the real app:
  the banner shows the host's plain message with no stray invisible-character prefix, the read-only latch (`deniedAsReadOnly`) engages
  for `readOnly`, and `forbidden` is treated as a refusal. Known low item in the notes: `assets-state.ts messageOf` does not strip the
  prefix, so check the asset browser's error text specially. Open DevTools in the editor and look at the raw error object if the text looks wrong.
- [ ] Connect as bob (editor): edits and Play work; Shutdown is refused with `forbidden`; as alice (admin) Shutdown works.
- [ ] A second client while one is connected: same user takes over; a higher role preempts (admin over editor over viewer); otherwise the
  error is "The host is serving another session (a viewer session); try again later" (code `busy`).
- [ ] Revoke without a restart: edit tokens.json (keep its owner and mode), then `kill -HUP $(pidof N2EditorHost)` or just reconnect.
  Host log: `Reloaded the tokens file: N user(s)`, and a user no longer granted is closed with
  `Closing the connection of user=... the reloaded tokens file no longer grants it`. The editor shows a disconnect, not a hang.
- [ ] `--read-only` host (no tokens needed): start `N2EditorHost --project /srv/proj --port 9999 --renderer software --read-only --token-env N2_EDITOR_TOKEN`
  (it cannot be combined with `--play` or `--create`). In the editor the read-only badge shows (`ServerInfo.readOnly`), writes and Play
  are disabled, and a forced write returns the `readOnly` code (the gate speaks first: on a read-only host even an admin's writes get
  `readOnly`, not `forbidden`).
- [ ] The connect form's optional User field works with a tokens-file host; the ssh "recent remotes" remember the user and never store the token.

## 5. Orphaned `.meta` cleanup: a decision, not a test (item 4)

Facts to decide on (`docs/editor.html`, "a rescan forgets deleted files but keeps their .meta"): when an asset file is deleted, a
rescan drops it from the index, but its `.meta` stays in `.import/`, on purpose, so a file that comes back (branch switch, save by
rename, undo in a file manager) keeps its import settings and, since UUIDs are path-derived, its UUID. A cleanup needs a `DeleteAsset`
command, which was left post-MVP (`docs/editor.html`: "DuplicateAsset, DeleteAsset and GetThumbnail are not part of E8"). `MoveAsset`
exists since protocol 1.24.0 and moves the `.meta` with the file.

Try this to see the size of the problem in a real project: delete a few assets in the editor or the file manager, then
`ls .import/` and count `.meta` files with no asset. Then decide:

- [ ] Option A, do nothing: orphans are harmless clutter in a committed `.import/`; revisit if projects accumulate hundreds.
- [ ] Option B, add `DeleteAsset` (a Write command, a viewer gets `forbidden`, a read-only host `readOnly`, a play host refuses) that
  deletes the file and its `.meta` together, plus a separate "prune orphaned metadata" action, never automatic (the reason for keeping them
  is that a file can vanish for a moment).
- [ ] Option C, only the prune action, no `DeleteAsset`.
Record the choice on #149; if B or C, it is a new issue (protocol change, so a minor version bump and a frontend change).

## What I could not verify

The whole Electron UI and its text (labels, badge placement, the tunnel's ssh arguments, how a launched host is started) because the
frontend repo is out of scope here; the real output of `ssh -L 127.0.0.1:0:...` (that is what step 2 measures); the Windows user data
folder for the TLS identity; any behaviour on real GPU hardware; whether Windows OpenSSH prints the allocated-port line. The smoke
script commands and the ready-line formats come from `tests/smoke/editor_host_smoke.py` and `editor-server/src/HostOptions.cpp`
(`FormatReadyLine`) and were not run here (no local runs were asked for).

No docs/ page was changed: there is no existing page that is the home of a live-host checklist (`docs/editor.html` documents the host
and its roles; its "Not verified" text already points at this gap), so none was invented.
