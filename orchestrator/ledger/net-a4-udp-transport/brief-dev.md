# Task net-a4-udp-transport: Netcode A4: native UdpTransport (Windows and Linux) implementing ITransport, loopback integration tests with the A3 channels, and refusal of non-loopback binds without encryption (--insecure-bind)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-a4-udp-transport.

## Issue / plan
Netcode design C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED 2026-10-20: own UDP layer, no new dependency; fail-closed binds: encryption and auth mandatory on any non-loopback bind, --insecure-bind to override (N4); the handshake and AEAD are A5, NOT here). MERGED: A3 #197 (the n2net core: ITransport, SimTransport, packet header, acks, reliable/unreliable channels, fragmentation; read engine/include/engine/network/n2net/* and docs/netcode.html), A1/A2 (settings/presets, SimulationClock). Read the design sections 2.1 (transport abstraction, 2.1.1 native), 2.1.3 (fail closed like the editor host), 1.3 threat model, 6 (testing) and 7.2 (A4), and the editor host's bind refusal and socket helpers (editor-server/: Connection, poll/WSAPoll usage, fail-closed bind from the earlier security work) FIRST. THIS ITEM IS A4 ONLY. SCOPE: (1) UdpTransport implementing ITransport over BSD sockets / Winsock (non-blocking, polled on the calling thread, no threads), IPv4 and IPv6 (dual-stack where available, documented), bind address/port options, max datagram size 1200 from A3, per-peer identification by address (peers are created on first valid datagram or explicit connect: follow A3's interface for connect/disconnect events), socket errors surfaced as data (never exceptions across the API), proper handling of WSAECONNRESET/ICMP unreachable on Windows UDP (ignore as the standard pitfall: SIO_UDP_CONNRESET), EWOULDBLOCK/EINTR, large receive buffers sized sensibly, source address validation, a hard cap on the number of tracked peers and per-peer state (DoS), amplification safety (never send more than received to an unauthenticated address: A5 adds the cookie handshake; here at minimum the transport must not reply to unknown sources by itself), (2) the FAIL-CLOSED BIND rule: binding a non-loopback address is refused unless the caller passes the insecure flag explicitly (the option name the design gives: --insecure-bind) with a loud one-time warning, and since A5 (encryption) does not exist yet the transport API takes an explicit 'encryption available' parameter that is false today: add tests, (3) NOT built for the web (no sockets): guard with the platform macros so the web and any platform without sockets compile (the library part stays; UdpTransport is native-only), (4) TESTS (all native legs: linux x86_64 Clang/GCC, arm64, windows x3; web only compiles): loopback integration: two UdpTransport endpoints on 127.0.0.1 with ephemeral ports exchanging A3 channel traffic (reliable-ordered, unreliable-sequenced, a fragmented large message) with and without simulated loss through a test shim, peer cap, source spoof/unknown-source handling, truncated and oversize datagrams, bind refusal for non-loopback without the flag (use an address on the machine: 0.0.0.0 counts as non-loopback), IPv6 loopback when available (skip cleanly if not), shutdown/re-bind, Windows connection-reset handling (Windows legs), bounded test run time (timeouts, no flaky sleeps: poll with deadlines), (5) DOCS: docs/netcode.html (the native transport, bind rules, platform notes, limits, what is not there yet: handshake/encryption A5, N2Server A6), docs/testing.html. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/

## Acceptance
- UdpTransport implements ITransport natively (IPv4/IPv6, non-blocking, no threads, peer caps, Windows connection-reset handling), not built for web
- Non-loopback bind refused unless --insecure-bind semantics are used explicitly; tested
- Loopback integration tests with A3 channels pass on all native legs with bounded run time
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: socket error handling on both platforms (connreset, EWOULDBLOCK, EINTR), no blocking calls, bounded peer state (DoS), amplification safety, fail-closed bind cannot be bypassed by 0.0.0.0/::/hostnames, IPv6 handling, flaky-test risk (ports, timing), web exclusion, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a4-udp-transport` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-a4-udp-transport` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a4-udp-transport/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-a4-udp-transport --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a4-udp-transport/dev.json: {"pr":<number>,"branch":"feat-net-a4-udp-transport","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-a4-udp-transport` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
