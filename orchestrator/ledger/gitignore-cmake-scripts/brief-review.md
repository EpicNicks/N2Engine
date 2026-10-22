# Review gitignore-cmake-scripts: Stop .gitignore from hiding new CMake scripts under cmake/
Read-only. Do NOT edit files, push, or comment on GitHub. Repo C:/Method Notes Obsidian/N2Engine. First `git fetch`, then review `git diff origin/master...origin/fix-gitignore-cmake-scripts` (read C:/Method Notes Obsidian/orchestrator/ledger/gitignore-cmake-scripts/dev.json for the PR number and the dev's deviations; if C:/Method Notes Obsidian/orchestrator/ledger/gitignore-cmake-scripts/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...` from inside C:/Method Notes Obsidian/N2Engine.

## Plan under review
Add `!cmake/*.cmake` next to the existing `!text/cmake/*.cmake` exception in .gitignore (the `*.cmake` rule currently hides new files in cmake/, which is why cmake/PhysXLinuxHashes.cmake had to be force-added in PR #120). Change only .gitignore; keep its line endings and layout; add a one-line comment. Verify with git check-ignore that cmake/PhysXLinuxHashes.cmake (tracked) and a hypothetical untracked cmake/Foo.cmake are NOT ignored, while generated files such as CMakeCache.txt, cmake_install.cmake and build output stay ignored, and that `git status` on the branch shows nothing new unexpectedly untracked. CI must stay green (no code change).

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; GL/GPU code CI can't run (say so); regressions. Say nothing about style.

## Output
Overwrite C:/Method Notes Obsidian/orchestrator/ledger/gitignore-cmake-scripts/review.json: {"head":"<origin/fix-gitignore-cmake-scripts sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
