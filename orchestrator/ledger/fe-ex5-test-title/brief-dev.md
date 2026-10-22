# Task fe-ex5-test-title: Frontend: test title still said protocol 1.15.0 after the 1.16.0 sync
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: fix-ex5-test-title.

## Issue / plan
Follow-up of merged frontend E-X5 (#54): src/test/engine-client-extensions.test.ts (~line 138) has a test title that says "the client is at 1.15.0" while its assertion was bumped to 1.16.0: fix the title (and grep src/ and mcp/ tests for any other leftover "1.15.0" in titles or comments describing the current protocol version; leave historical statements about 1.15.0 features alone). Tiny item: one commit. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo is READ-ONLY reference. npm run typecheck and npm test must pass locally before you push; CI has 3 checks. NEVER run taskkill; no attribution lines on commits or PRs; GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; merge origin/master into the branch if it moved before your final push; your FINAL message must be a real report (PR number, head sha, what you did, deviations).

## Allowed paths (regex, repo-relative)
- ^src/
- ^mcp/
- ^README\.md$

## Acceptance
- No test title or comment names the wrong current protocol version
- typecheck, tests and 3 CI checks green

## Notes
Reviewer: only titles/comments changed, no assertion changed.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-ex5-test-title` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-test-title/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-ex5-test-title --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-test-title/dev.json: {"pr":<number>,"branch":"fix-ex5-test-title","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-ex5-test-title` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
