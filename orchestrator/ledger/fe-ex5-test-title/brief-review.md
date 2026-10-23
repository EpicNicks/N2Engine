# Review fe-ex5-test-title: Frontend: test title still said protocol 1.15.0 after the 1.16.0 sync
Read-only. Do NOT edit files, push, or comment on GitHub. Repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend. First `git fetch`, then review `git diff origin/master...origin/fix-ex5-test-title` (read C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-test-title/dev.json for the PR number and the dev's deviations; if C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-test-title/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...` from inside C:/Method Notes Obsidian/N2Engine-Electron-Frontend.

## Plan under review
Follow-up of merged frontend E-X5 (#54): src/test/engine-client-extensions.test.ts (~line 138) has a test title that says "the client is at 1.15.0" while its assertion was bumped to 1.16.0: fix the title (and grep src/ and mcp/ tests for any other leftover "1.15.0" in titles or comments describing the current protocol version; leave historical statements about 1.15.0 features alone). Tiny item: one commit. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo is READ-ONLY reference. npm run typecheck and npm test must pass locally before you push; CI has 3 checks. NEVER run taskkill; no attribution lines on commits or PRs; GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; merge origin/master into the branch if it moved before your final push; your FINAL message must be a real report (PR number, head sha, what you did, deviations).

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; line endings; regressions. Say nothing about style.

## Output
Overwrite C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-test-title/review.json: {"head":"<origin/fix-ex5-test-title sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
