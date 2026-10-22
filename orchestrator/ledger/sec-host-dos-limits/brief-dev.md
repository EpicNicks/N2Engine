# Task sec-host-dos-limits: Editor host security S1: count failed pre-Hello connections, 2 s pre-Hello budget, idle/write/session deadlines after Hello (audit F1, F2)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-host-dos-limits.

## Issue / plan
Source: the security audit C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md. Read findings F1 and F2 and section 6 items 1 and 2 fully first; F6 explains why loopback is treated specially. Line numbers below are from origin/master at a1aa1005; re-find the code at current master before you edit.

THREATS CLOSED.
- F1 (High, unauthenticated attacker A1): the server serves one connection at a time (`listen(_listenSocket, 1)`, EditorServer.cpp ~1550; ServerLoop ~2047-2172 runs HandleClient inline on the only network thread). A peer that connects and sends nothing, or stalls the TLS handshake, holds the thread for the whole pre-Hello budget (DefaultHelloTimeout 5000 ms, EditorServer.hpp ~203). It is never counted: only a token-refused Hello calls RateLimiter::RecordFailure (the `else if (_helloTokenRefused)` branch, EditorServer.cpp ~2409-2414). A Hello timeout only logs (logIfHelloTimedOut, ~2254-2258); an early close is not seen at all; a TLS handshake failure or timeout returns false from BeginTls (~2175-2224) and ServerLoop just closes. A reconnect loop (`while true; do nc host 9999 & sleep 4.9; done`) keeps every legitimate user out.
- F2 (High, any authenticated role, including viewer A2): once Hello succeeds, HandleClient's deadline() returns nullopt (~2243-2249), so Receive waits forever. Send (~6833-6866) calls WaitUntilReady(socket, true) with no deadline, so a client that stops reading (for example after a ~3.7 MB RenderFrame) blocks the thread for ever. A host with no token treats every connection as a session from accept. Either way, one idle or non-reading client locks out every admin and editor until the host restarts.

SCOPE.
(a) Pre-Hello accounting (F1), on a host that has a RateLimiter (one that requires an access token; a host without authentication has no failed-Hello concept, so nothing changes there):
- Count every connection that ends without a successful Hello as a failure for its address. That covers the pre-Hello deadline expiring, the peer closing before Hello, a non-Hello command before Hello, an oversized pre-Hello frame, and a TLS handshake that failed or timed out (count it in ServerLoop where BeginTls returns false). Use the existing RecordFailure path with no user named, so the existing backoff, log-once-per-ban and suppressed-count behaviour apply unchanged.
- Keep not counting a Hello refused only for a protocol version mismatch: that is a good-faith old client, and RateLimitServerTest.OnlyAWrongTokenCountsNotAVersionMismatch must still pass.
- Loopback (127.0.0.0/8, ::1) stays exempt from per-address bans. Do NOT feed loopback pre-Hello failures into the global loopback backoff: behind an SSH tunnel or a TLS proxy every peer is loopback, and counting a mere connect would make the F6 lockout cheaper. Loopback relies on the shorter budget instead. Document this as a remaining gap.
- Cut DefaultHelloTimeout from 5000 ms to 2000 ms for every peer (the TLS handshake and Hello share it, as today). The prior-offender budget (RateLimitPolicy::priorOffenderBudget, 2000 ms today) must stay no longer than the normal one. Either lower it (for example to 1000 ms) or keep it equal and say the distinction no longer applies; pick one, justify it in the PR, and document it.
- Check that a real client still fits in 2 s: the Electron client over TLS with an SSH tunnel, and the play child launched by the editor. The play child's launcher connects locally.

(b) Post-Hello deadlines (F2). These apply to every session, including a host without a token, where the session starts at accept.
- Idle read deadline: close the connection when no complete frame has arrived from the client for `idleTimeout`. Default 5 minutes. Every received frame resets it, including the polled commands (PollEvents, GetAudio, RenderFrame and the other read commands), so the editor's long-lived polling session, viewport frame streaming, audio, and the editor's session with the play child are unaffected.
- Write deadline in Send, on both the plain and the TLS path (the TLS WantWrite branch also waits with no deadline today): fail the Send when the socket makes no write progress for `writeTimeout`. Default 30 s. Reset the deadline on every partial write, so a slow but reading client on a large frame is not cut off. On expiry, close the connection and log exactly one line, including peer=.
- Optional max session age, default off: close a session older than this.
- Configuration: EditorServer setters, refused while serving like the existing SetHelloTimeout and SetRateLimitPolicy, plus HostOptions flags with documented defaults. For example `--idle-timeout <seconds>` (0 disables it, for test rigs and debugging), `--write-timeout <seconds>` and `--max-session <minutes>` (default off). Update the --help text and HostOptionsTests for each flag (valid values, 0, negative or garbage values refused).
- Every close for a deadline logs one line naming the reason and the peer, never a token.

(c) Time. The RateLimiter already takes an injected clock (RateLimiter::Clock, set with SetRateLimitClock). The socket deadlines (helloDeadline, Receive and WaitUntilReady) read std::chrono::steady_clock::now() directly. Route the pre-Hello, idle, write and session deadlines through one injectable clock: a new SetConnectionClock, or reuse the rate-limit clock if that is clean. WaitUntilReady already re-checks the deadline every PollIntervalMicroseconds, so advancing a fake clock fires a deadline within one poll interval, and tests never sleep. Where a socket test truly needs real elapsed time, keep it under about 100 ms. No test may sleep longer than that.

TESTS (tests/editor-server: RateLimitServerTests.cpp, EditorServerHardeningTests.cpp, HostOptionsTests.cpp; tests/common/TestSockets.hpp may be extended):
1. An idle pre-Hello peer is closed at the deadline and counted; after freeFailures+1 idle connections, the address is refused at accept (backoff) under the fake clock.
2. A peer that closes before Hello is counted.
3. A stalled TLS handshake is counted. Guard this test like the existing TLS tests (N2ENGINE_EDITOR_TLS ON only).
4. A version-mismatch Hello is still not counted.
5. Loopback idle pre-Hello connections are not banned per address and do not start the global loopback backoff. The existing loopback global rule for wrong tokens is unchanged.
6. An authenticated idle client is disconnected at the idle deadline (fake clock), and one line is logged.
7. A client that keeps polling (PollEvents every simulated few seconds for longer than the idle timeout) stays connected.
8. A client that never reads cannot block the server past the write deadline. Shrink SO_RCVBUF on the client (and SO_SNDBUF on the accepted socket through a test hook if needed), then request responses larger than the buffers without reading. A large ReadTextAsset in the project test harness works, or any response big enough. After the deadline, the server is free and a second client connects and gets ServerInfo.
9. The max session age closes a session when set, and never when off.
10. A host without a token: an idle session is closed by the idle deadline, and no rate limiter exists (the existing test ARateLimitingHostWithoutAnAccessTokenHasNoFailedHellosAndNoLimiter still holds).
11. All existing RateLimitServerTests, RateLimiterTests, EditorSessionTests, DroppedClientTests, ViewportFrameTests, AudioStreamProtocolTests and play tests pass unchanged, or with only a timeout constant updated (say which).

DOCS.
- docs/editor.html and docs/logging-and-editor.html: the new limits and their defaults, the flags, and the log lines. Update the rate-limit pages: the existing text says idle connections are not backed off (#rate-limit-gaps), so correct it.
- State plainly what is still NOT covered. The serial single-client model remains: a peer can still hold the thread for up to 2 s per connection, and loopback peers (behind a proxy or tunnel) are not counted for idle connects. The structural fix is the pre-auth slots design (audit item 3), not done here.
- Add docs/testing.html rows for the new tests.
- deploy/: touch it only if a unit or the Dockerfile must mention a new flag (for example a comment). Otherwise leave it alone.

OUT OF SCOPE: no protocol change (protocol.json untouched); no frontend change; no pre-auth slot pool (audit item 3); no F6 rate-limiter redesign; no log coalescing (F7); and no fail-closed bind (item sec-bind-fail-closed, which is gated on this one).

CI. Nothing builds locally; CI is the build. The Tests workflow has 8 checks, and the required one is `linux (Release, x86_64, Clang)`. Every leg must be green, the Windows and TLS legs included.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^tests/editor-server/
- ^tests/common/TestSockets\.hpp$
- ^docs/
- ^deploy/

## Acceptance
- All Tests-workflow jobs green (8 checks; required linux (Release, x86_64, Clang))
- Idle, early-closed and TLS-handshake-failed pre-Hello connections count as per-address failures; loopback exemption and the global loopback rule unchanged; version-mismatch Hello still not counted
- Pre-Hello budget 2 s for all peers; prior-offender budget never longer, documented
- Idle read deadline (default 5 min, disable-able), Send write deadline (no-progress, default 30 s) on plain and TLS paths, optional max session age (default off), each with a HostOptions flag and tests
- Socket tests use an injected clock or real waits under about 100 ms; a polling client is unaffected
- No protocol change; docs state the limits, flags, defaults and what is still not covered
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive (audit F1/F2). The reviewer must try to break it: (1) find any pre-Hello exit path that is still not counted (an early close mid-header, a mid-payload close, an oversize frame, a non-Hello command, a TLS failure versus a TLS timeout, an exception path); (2) confirm loopback connects do not feed the global loopback backoff, since that would make the F6 lockout cheaper; (3) look for a way to keep a session alive without real activity that defeats the idle deadline, which is acceptable only if it requires valid credentials, and say so; (4) check that the write deadline covers the TLS WantWrite path and resets on progress, so a slow-but-reading 3.7 MB frame over a slow link is not killed while a non-reader is; (5) check that every deadline uses the injected clock with no real sleeps over about 100 ms, and that the setters are refused while serving; (6) check the play child and --exit-on-disconnect: an idle close of the editor's play session ends the play child, so confirm the editor's polling keeps it alive. Unverified, cannot be checked from CI: the Electron client's polling cadence when its window is hidden or minimized (frontend repo); whether 2 s is enough over a high-latency SSH tunnel with TLS. Expect small textual merges with sec-scene-depth-cap (EditorServer.cpp, docs/testing.html) and sec-lua-budget (Host.cpp, HostOptions, docs). These may run in parallel; sec-bind-fail-closed is gated on this item.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-host-dos-limits` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-host-dos-limits` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-host-dos-limits/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-host-dos-limits --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-host-dos-limits/dev.json: {"pr":<number>,"branch":"sec-host-dos-limits","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-host-dos-limits` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
