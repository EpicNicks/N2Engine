# Task docs-preauth-leftover-lows: Docs and comments: remaining stale pre-auth wording (one session locks everyone out, every peer behind a proxy is loopback)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: docs-preauth-leftover-lows.

## Issue / plan
Docs and comment-only item, no behaviour change. Four known leftovers from the reviews of #180 and #182, each to be verified against the CODE (EditorServer.cpp acceptor/pre-auth workers/session thread, RateLimiter.cpp, PreAuthSlots.hpp, Host.cpp) before editing: (1) docs/logging-and-editor.html ~line 716: 'serves one session at a time, so a connection that never lets go locks every other client out' and the list of a silent peer / a stalled TLS handshake: since PR #176 a silent or stalled PRE-AUTH peer holds only one pre-auth slot (the same paragraph says so two sentences later); a connected authenticated SESSION that never lets go does still occupy the session thread until its deadlines/takeover/preemption (check exactly which deadlines apply, per RunSession/SessionLoop): rewrite so every clause is true. (2) docs/logging-and-editor.html ~649 and ~678 and editor-server/include/editor-server/PreAuthSlots.hpp ~25-30 ('Useless behind a proxy'): 'behind an SSH tunnel or a reverse proxy every peer is loopback' is only true when the proxy/tunnel connects over loopback (an SSH tunnel does; a proxy or container on a non-loopback network presents its own address): say that, matching the corrected text already at ~598 and in Host.cpp; check what 'useless' refers to (the per-address limit) and make the sentence precise. Then grep the whole repo (docs/, editor-server/, engine/, deploy/, README.md, tests/ comments; for tests/ change only comments that are FALSE) for further instances of the same two stale claims and fix them the same way. Re-read every edit (HTML well-formed; balanced C++ comment markers; CRLF preserved in files that have CRLF). Comment-only C++ edits, native behaviour byte-identical.

## Allowed paths (regex, repo-relative)
- ^docs/
- ^README\.md$
- ^deploy/
- ^editor-server/
- ^engine/
- ^tests/

## Acceptance
- The four leftovers corrected, each verified against the code, plus any siblings found by grep
- C++ edits are comments only; HTML well-formed; all checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: C++ hunks comment-only (strip comments and compare), CRLF preserved, EVERY changed sentence true against the code (what a connected session can hold, which deadlines, proxy/loopback conditions), grep for remaining instances of both stale claims, count mediums as findings.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs docs-preauth-leftover-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref docs-preauth-leftover-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/docs-preauth-leftover-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head docs-preauth-leftover-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/docs-preauth-leftover-lows/dev.json: {"pr":<number>,"branch":"docs-preauth-leftover-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/docs-preauth-leftover-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
