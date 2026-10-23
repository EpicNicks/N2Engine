# Task physx-release-flags: physx-linux workflow: create dependency releases as prereleases, never as the repo's latest release
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-physx-release-flags.

## Issue / plan
The user said (2026-10-13): make sure the releases are appropriately tagged because they are not engine releases. The repo convention is that every release is a prerelease (Box-Demo, physx-dependencies, and now physx-dependencies-linux-5.6.1 after the orchestrator edited it). In .github/workflows/physx-linux.yml the publish job creates the release with `gh release create ... --title ... --notes ...` (~l.249-252): add `--prerelease --latest=false`, change the title to `PhysX ${PHYSX_VERSION} for Linux (build dependency, not an engine release)`, and the notes to: `Build dependency, not an N2Engine release.` followed by one sentence that these are prebuilt static Release libraries of NVIDIA PhysX built by the PhysX Linux workflow and downloaded by the engine CMake with a pinned SHA256 (cmake/PhysXLinuxHashes.cmake), and that the archives carry the PhysX LICENSE.md. Also add the same wording as a comment above the step so the intent survives. Nothing else changes: no trigger, permission, action version or build step changes. Do NOT publish or dispatch anything. Verify by reading the file that the YAML still parses (python -c yaml via a quick check or careful inspection) and that the dry-run (pull_request path trigger) still runs and passes on CI.

## Allowed paths (regex, repo-relative)
- ^.github/workflows/physx-linux.yml$

## Acceptance
- The PhysX Linux dry run still passes
- The release-creation command uses --prerelease --latest=false and the dependency wording

## Notes
Tiny. The already-published release was fixed by hand (prerelease, not latest, renamed, new body).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs physx-release-flags` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-physx-release-flags` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/physx-release-flags/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-physx-release-flags --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/physx-release-flags/dev.json: {"pr":<number>,"branch":"fix-physx-release-flags","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-physx-release-flags` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
