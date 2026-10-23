# Task net-b4-input-frames: Netcode B4: networked input layout, InputFrame, redundant client inputs, server jitter buffer
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b4-input-frames.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read the whole document, the sections for this stage and the stage table row, then the MERGED code: n2net (transports, SecureTransport, channels, the network simulator and its tests), N2Server, B1 (NetworkObject, world/server/client classes, fingerprint) and B2 (field replication: .Replicated(), codecs, delta vs acked baseline, NetworkTransform) in engine/src/network, A2 (simulation clock, fixed tick, StepTicks, TickOptions), and the existing network docs page. Follow the security rules of the design (bounded counts and sizes before allocation, rate limits, authority checks, no client-controlled allocation). The web build must still compile (n2net core is compiled for web; native sockets are not). Nothing builds locally: re-read every edit twice and trace identifiers. SCOPE (stage B4 only): (1) an input layout declaration (a small schema of named axes/buttons/vectors with bit widths and quantisation, declared in C++ and from Lua, registered in the fingerprint from B1 so client and server must agree) and the InputFrame type (tick, sequence, packed payload); (2) client side: sample the local input each fixed tick into an InputFrame, keep a bounded ring of unacked frames, send each datagram with the last N frames (redundancy, N from settings) on the unreliable channel so loss does not lose inputs, acks from the server trim the ring; (3) server side: per client jitter buffer keyed by tick (bounded depth), duplicate/old/too-far-future frames rejected, a configurable input delay target, behaviour when an input is missing (repeat last input N ticks then neutral input; counters exposed), rate limit and size cap per client per second, authority: input is applied ONLY for the client's own controlled object(s) as assigned by the server; (4) a server API for the game to read the input of the controlled object for the current tick (C++ and Lua), deterministic given the same frames; (5) tests on the simulator: loss and reorder do not drop inputs thanks to redundancy, jitter buffer ordering and bounds, flood rejected, missing input policy, quantisation round trip, determinism of encode; (6) docs true against the code. NO prediction/reconciliation here (B6). Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

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
- Input layout schema in the fingerprint; InputFrame encode/decode bounded
- Redundant sends and acks; server jitter buffer with bounds and missing-input policy
- Input applied only to the client own controlled object; rate/size limits
- Simulator tests, docs true, CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security): every count/size bounded, clients cannot drive objects they do not control, ring/jitter-buffer memory bounded under flood, tick wraparound, quantisation round trip, Lua layout validation, compile-level correctness, web build.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b4-input-frames` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b4-input-frames` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b4-input-frames/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b4-input-frames --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b4-input-frames/dev.json: {"pr":<number>,"branch":"feat-net-b4-input-frames","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b4-input-frames` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
