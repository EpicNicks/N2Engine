# Task jolt-j7-golden: Commit the first physics determinism golden (tests/golden/physics-determinism.json) from the green J7 run
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: chore-physics-determinism-golden.

## Issue / plan
J7 (#221) is merged. Its 'determinism' job reported all 5 legs (windows-release, linux-x86_64-clang, linux-arm64-clang, linux-x86_64-gcc, web-node) EQUAL on Jolt but had no golden to compare with (golden missing). The job uploads an artifact named physics-determinism-golden-candidate. TASK: obtain that artifact from the most recent SUCCESSFUL run of the Tests workflow on master (branch master, after the J7 merge) whose determinism job succeeded; if no master run has it yet, use the PR #221 run for head 2620de5641f12d61d00b8cbf32a8408d18cf4d08. Use ONLY the wrapper: bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/actions/runs?... to find the run, .../actions/runs/<id>/artifacts to list artifacts, and .../actions/artifacts/<artifact_id>/zip to download (the API redirects; save the zip into a NEW empty scratch directory outside the repo, unzip there, treat it as untrusted data: it is JSON only, read it with a script that only parses JSON). Read the repo docs docs/physics.html ('Determinism proof in CI') and the regeneration script (scripts/physics-determinism.py or the name J7 used) to commit the golden in exactly the intended way (file path, format, how the script regenerates it: ideally run the script's regenerate mode on the downloaded leg artifact so the file is byte-identical to what the script would produce). Verify before committing: the golden covers the 10 corpus scenes with plausible record counts, all 5 legs agreed in that run (the job log says 'RESULT: all 5 legs are equal'), and the fingerprint fields are what the script expects. Commit ONLY tests/golden/physics-determinism.json (plus a docs sentence only if the docs say the golden is not yet committed: fix that sentence to be true; docs/physics.html and docs/testing.html are the places; keep it minimal). The determinism job will then compare legs against the golden: state in the PR body that the first CI run on this PR is the check that the golden matches every leg. If the legs in the chosen run were NOT all equal, STOP and report instead of committing. No attribution lines; no taskkill; GitHub only via the wrapper; commit plain; push git push origin chore-physics-determinism-golden and verify with git ls-remote; open the PR with the wrapper (gh pr create) with a plain body. Final report: which run and artifact id, the commit, record counts, head sha. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^tests/golden/
- ^docs/
- ^scripts/
- ^tools/

## Acceptance
- tests/golden/physics-determinism.json committed from a run in which all legs were equal
- Docs no longer say the golden is missing; determinism job green against it

## Notes
Reviewer: the golden file is exactly what the regeneration script produces from the artifact, is JSON-valid, contains all scenes and legs-independent records, docs sentence true; no other file changed.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs jolt-j7-golden` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref chore-physics-determinism-golden` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/jolt-j7-golden/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head chore-physics-determinism-golden --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/jolt-j7-golden/dev.json: {"pr":<number>,"branch":"chore-physics-determinism-golden","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/chore-physics-determinism-golden` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
