# Task cleanup-hostcpp-comments-application-docs: Tidy: stale comments in editor-server/src/Host.cpp (one-at-a-time server, everything behind a proxy is loopback) and the docs/application.html Init sentence that predates the three settings Init reads
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-hostcpp-comments-app-docs.

## Issue / plan
Small cleanup, comments and docs only, no behaviour change. (1) editor-server/src/Host.cpp around lines 486-492: the comment 'The deadlines that keep one connection from holding the one-at-a-time server' is stale since PR #176 (acceptor + pre-auth workers + a session thread; a pre-Hello connection holds one pre-auth slot, the deadlines bound how long) and 'Everything behind a proxy is loopback, which has no per-address limits (see the docs)' is wrong in general: the host sees the peer address the proxy/container presents, which is loopback only when the proxy connects over loopback (docs/editor.html and docs/logging-and-editor.html, 'behind a proxy' text, now say the right thing since PR #180: read them and match them, verify against EditorServer.cpp/RateLimiter.cpp/PreAuthSlots.hpp/Host.cpp). Search editor-server/ and engine/ for any OTHER comment with the same stale claims (grep 'one-at-a-time', 'one at a time', 'serial', 'serving thread', 'loopback backoff', 'shared backoff') and correct only comments that are verifiably stale; do not touch code. (2) docs/application.html around line 119 (the Config::ApplicationOptions table, row projectPath and the surrounding prose): 'Init does not report a bad value of either' (or similar) predates Init reading THREE project settings (determinism.luaSeed, math.simdTier and the third one: read Application::Init in engine/src/Application.cpp to see exactly which and what Init does with a bad value of each: report/ignore/default); rewrite the sentence so it is TRUE for all three, verified against the code, and check the 'four fields' wording of the table against the struct in the header. Re-read every edit (comments must still compile: balanced comment markers). Native behaviour must be byte-identical (comment-only C++ edits). No other changes. USER-APPROVED WIDENING (2026-10-20): also fix docs/logging-and-editor.html (the --project row around line 219 says Init reads projectPath only for physics.gpu: it reads four settings, verify against Application::Init and match docs/application.html) and the user-visible --help strings in editor-server/src/HostOptions.cpp (--no-rate-limit, --preauth-slots ~517-534) that say everyone behind a proxy is loopback: correct them to what the code does (loopback only when the proxy connects over loopback), text only, and CHECK that no test or doc compares the --help output (grep tests/ and docs/ for those strings; update any that quote them).

## Allowed paths (regex, repo-relative)
- ^editor-server/src/
- ^editor-server/include/
- ^engine/src/
- ^engine/include/
- ^docs/application\.html$
- ^docs/logging-and-editor\.html$

## Acceptance
- Host.cpp comments (and any sibling stale comments found) match the post-#176/#177 code
- docs/application.html Init sentence true for all three project settings, verified against Application::Init
- Comment-only C++ edits; all checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: C++ diff is comments only (strip comments and compare, or inspect each hunk), comment syntax balanced, every changed sentence true against the code, docs sentence true for all three settings.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs cleanup-hostcpp-comments-application-docs` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-hostcpp-comments-app-docs` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/cleanup-hostcpp-comments-application-docs/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-hostcpp-comments-app-docs --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/cleanup-hostcpp-comments-application-docs/dev.json: {"pr":<number>,"branch":"fix-hostcpp-comments-app-docs","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-hostcpp-comments-app-docs` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
