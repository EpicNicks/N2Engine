# Review _selftest: selftest
Read-only. Do NOT edit files, push, or comment on GitHub. Repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend. First `git fetch`, then review `git diff origin/master...origin/f9-bom-sync` (read C:/Method Notes Obsidian/orchestrator/ledger/_selftest/dev.json for the PR number and the dev's deviations; if C:/Method Notes Obsidian/orchestrator/ledger/_selftest/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...` from inside C:/Method Notes Obsidian/N2Engine-Electron-Frontend.

## Plan under review
self-test only

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; line endings; regressions. Say nothing about style.

## Output
Overwrite C:/Method Notes Obsidian/orchestrator/ledger/_selftest/review.json: {"head":"<origin/f9-bom-sync sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
