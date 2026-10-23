# Task net-b3-clock-interpolation: Netcode B3: clock sync and snapshot interpolation of remote entities
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b3-clock-interpolation.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read the whole document, the sections for this stage and the stage table row, then the MERGED code: n2net (transports, SecureTransport, channels, the network simulator and its tests), N2Server, B1 (NetworkObject, world/server/client classes, fingerprint) and B2 (field replication: .Replicated(), codecs, delta vs acked baseline, NetworkTransform) in engine/src/network, A2 (simulation clock, fixed tick, StepTicks, TickOptions), and the existing network docs page. Follow the security rules of the design (bounded counts and sizes before allocation, rate limits, authority checks, no client-controlled allocation). The web build must still compile (n2net core is compiled for web; native sockets are not). Nothing builds locally: re-read every edit twice and trace identifiers. SCOPE (stage B3 only): (1) client/server clock synchronisation over the existing secure channel (ping/pong style samples with the server tick and a monotonic client clock; filtered offset and RTT estimate using the best-of-N or a median filter, drift handling, never jumps backwards; bounded rates, malformed input rejected; estimates exposed to C++ and Lua as server time/tick, RTT, jitter); (2) snapshot interpolation of remote entities: the client buffers received NetworkTransform states per object with their server ticks, renders remote objects at (estimated server time - interpolation delay) interpolating position/scale linearly and rotation by slerp, with a configurable delay (default from settings: a few snapshot intervals), extrapolation limited and clamped when a snapshot is late, teleport flag respected (no interpolation across a teleport), buffer bounded (no unbounded growth under a flood or long stalls), out-of-order and duplicate snapshots handled; the locally controlled object (owner/prediction) is NOT interpolated (that is B6): provide the hook; (3) tests on the network simulator (latency, jitter, loss, reorder): clock estimate converges and stays within bound; interpolated position is smooth (bounded second difference) under jitter; teleport; late snapshot extrapolation clamp; buffer bounds; deterministic given a seeded simulator; (4) docs: the clock/interpolation section of the netcode docs and scripting-lua.html for the Lua accessors, true against the code. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^net/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/
- ^tools/
- ^n2server/
- ^apps/
- ^server/
- ^lua_project/
- ^[^/]*\.lua$

## Acceptance
- Clock sync converges, never goes backwards, bounded rates; exposed to C++ and Lua
- Snapshot interpolation with delay, bounded extrapolation, teleport, bounded buffers
- Simulator tests incl. jitter/loss/reorder; docs true; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: monotonic clock estimator, no division by zero or NaN at startup (first samples), buffer and rate bounds, tick wraparound handling (u32 ticks), slerp of non-normalised or NaN rotations, interpolation never applied to the locally controlled object, compile-level correctness, web build.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b3-clock-interpolation` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b3-clock-interpolation` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b3-clock-interpolation/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b3-clock-interpolation --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b3-clock-interpolation/dev.json: {"pr":<number>,"branch":"feat-net-b3-clock-interpolation","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b3-clock-interpolation` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
