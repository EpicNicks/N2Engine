# Task sec-preauth-s5-docs: Pre-auth S-5: docs and deploy notes for the bounded pre-auth slots, preemption/takeover and busy (docs only, verified against the code)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: docs-preauth-s5.

## Issue / plan
DOCS-ONLY item (no code change): PR S-5 of C:/Method Notes Obsidian/orchestrator/proposals/security-design-lua-editmode-and-preauth-slots.md PART 2 (read 2.1-2.8, especially 2.4 and the 2.6 line for S-5). The code is merged: S-1 #168 (Connection refactor), S-2 #171 (policy classes), S-3 #176 (acceptor + 4 workers + session thread, --preauth-slots, Busy plain Error, coalesced pre-auth logs), S-4 #177 (preemption by a strictly higher role, same-user takeover, no-token newest-takes-over, ErrorCoded busy = protocol 1.13.0, loopback backoff REMOVED in favour of a concurrency cap, TCP keepalive). FIRST audit what S-3 and S-4 already documented (grep docs/ for preauth, pre-auth, busy, takeover, slots, rate-limit, rate limit, Hello timeout, keepalive, proxy; read docs/editor.html, docs/logging-and-editor.html (anchor #rate-limit-gaps), docs/testing.html, any deploy/docker/linux docs pages, README flags tables) and write ONLY what is missing or stale. Expected gaps, each to be CONFIRMED against the code (EditorServer.cpp, Host.cpp, the pre-auth policy classes, the CLI/option parsing; never copy neighbouring docs): (1) the stale sentence 'a peer can hold the thread for the Hello timeout' (and any similar claim about one slow peer blocking the host) in logging-and-editor.html#rate-limit-gaps and anywhere else: replace with what is true now, and list the REMAINING gaps honestly (e.g. rate-limit gaps that still exist); (2) the flags/options: --preauth-slots (range, default) and any rate/handshake/keepalive knobs that exist: exact names, ranges, defaults, in the host command-line reference wherever flags are listed; (3) behaviour BEHIND A PROXY / port-forward / docker bridge / SSH tunnel: the host sees the proxy's address, so every client looks like one (often loopback) address and per-address limits and the loopback concurrency cap apply to everyone together; say what an operator should do (and what the code actually supports; do not invent a forwarded-for header feature); (4) the busy / preemption / takeover semantics from the operator and user point of view (who preempts whom, same-user takeover wait, what the client sees: ErrorCoded busy with holder role when the client asked for errorCodes, plain Error otherwise), and the protocol 1.13.0 note if the protocol doc is missing it; (5) deploy notes in the Linux/docker docs if they describe connection limits (container resource sizing: 4 workers + session thread + acceptor). Keep each addition short and factual; match the surrounding docs' style and structure (look at how other callouts and tables are written); update docs/index.html design-decision rows only if an existing convention says S-3/S-4 style changes get one and it is missing. Every sentence must be verified against CODE (this has cost many review rounds); where something could not be verified say nothing rather than guess. Docs-only: CI skips the build legs. If you find a real code bug while verifying, do NOT fix it: report it in dev.json notes/needsUser.

## Allowed paths (regex, repo-relative)
- ^docs/
- ^README\.md$
- ^deploy/

## Acceptance
- Stale pre-auth claims removed or corrected; remaining gaps listed honestly
- Flags, proxy behaviour, busy/preemption/takeover and deploy sizing documented where missing, each verified against the code
- Docs-only; all checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: verify EVERY new or changed sentence against the code (flag names/ranges/defaults, who preempts whom, takeover wait, keepalive, what is logged), that no claimed behaviour is invented (proxy forwarded-for, per-address limits), no stale claim remains anywhere in docs/ or README, and the pages stay consistent with each other.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-preauth-s5-docs` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref docs-preauth-s5` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-preauth-s5-docs/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head docs-preauth-s5 --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-preauth-s5-docs/dev.json: {"pr":<number>,"branch":"docs-preauth-s5","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/docs-preauth-s5` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
