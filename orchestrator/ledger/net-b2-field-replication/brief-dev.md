# Task net-b2-field-replication: Netcode B2: field replication (.Replicated(), Lua replicated = true, FieldKind codecs, delta vs acked baseline, NetworkTransform)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b2-field-replication.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read the field replication and snapshot sections (reflection-driven replication through FieldInfo/ComponentSerializer, .Replicated() on RegisterMember, Lua 'replicated = true', per-FieldKind codecs and quantisation, delta against the last ACKED baseline, NetworkTransform, bandwidth notes) and row B2 of the stage table (line ~297), and the security rules (bounded sizes, authority checks). B1 is merged (NetworkObject, net ids, spawn/despawn replication, fingerprint, world/server/client classes in engine/src/network and n2server): read it and its tests FIRST, plus FieldInfo.hpp (FieldKind, FieldInfo built by RegisterMember(...).Range().Tooltip().AsColor()), ComponentSerializer, ComponentRegistry::Describe, ValidateFieldValue/ClampFieldValue, how Lua components expose fields (GetLuaFields kind rules), and the n2net channel layer (reliable + unreliable channels, packet layer). SCOPE: (1) a '.Replicated()' flag on field registration (C++) and 'replicated = true' in a Lua component's field declaration; the replicated field set is part of the registry fingerprint from B1 so client and server must agree (fingerprint mismatch already refuses the join: extend the fingerprint to include replicated field ids/kinds); (2) wire codecs per FieldKind (bool, ints, float with optional quantisation from Range(), Vector2/3/4, Quaternion smallest-three, color, string with a cap, enums, AssetRef by id/path hash, GameObject references by net id with a sentinel for unknown): bounded decode (every length capped before allocation), exact round-trip tests, fuzz-style malformed-input tests; (3) server-side state replication: per client a baseline of last ACKED state per object per field; each tick (or at the configured send rate from the settings) send deltas (changed fields via a bitmask) on the unreliable channel with sequence + ack, resend based on acks (no per-packet guarantees: the next delta is computed against the last acked baseline), full state on spawn and on baseline loss, interest = everything (interest management is B9); (4) client applies updates to its local objects ONLY for replicated fields and only from the server; clients can never write replicated fields of others and the server ignores any replication data from clients; authority/ownership per the design for owner-writable fields is NOT in B2 (server-only authority); (5) NetworkTransform component: replicates position/rotation/scale (quantised per settings), applied on the client directly (interpolation is B3), with a teleport flag; (6) tests on the n2net network simulator (loss, reorder, duplication, latency): convergence under loss, delta correctness against the acked baseline, bandwidth smaller than full-state resend in a scripted scenario, determinism of encoded bytes (same state => same bytes), plus the multi-process loopback integration test extension (client observes server-side field changes through N2Server with real UDP); (7) docs: the replication section of the netcode docs, scripting-lua.html (replicated = true), serialization.html (.Replicated()) verified against the code. Respect: web build must still compile (n2net core is compiled for web), no unordered iteration in encoding, one RNG draw per statement. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

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
- .Replicated() and Lua replicated = true; fingerprint covers replicated fields
- Bounded codecs for every FieldKind with round-trip and malformed-input tests
- Delta against acked baseline converges under loss/reorder/duplication on the simulator; server-only authority
- NetworkTransform replicates; loopback integration extended; docs true; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security/protocol): every decode length/count bounded before allocation, client data never written to the world, baseline/ack logic cannot desync or leak memory (bounded history), codec byte-determinism, fingerprint covers what changes the wire format, quantisation round-trip error bounds, Lua replicated flag validation, web build still compiles, compile-level correctness.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b2-field-replication` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b2-field-replication` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b2-field-replication/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b2-field-replication --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b2-field-replication/dev.json: {"pr":<number>,"branch":"feat-net-b2-field-replication","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b2-field-replication` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
