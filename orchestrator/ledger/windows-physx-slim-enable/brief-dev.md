# Task windows-physx-slim-enable: Windows CI and CMake download the slim PhysX archive with a pinned SHA256
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-windows-physx-slim-enable.

## Issue / plan
PUBLISHED 2026-10-13 (prerelease `physx-dependencies-windows-5.6.1` on EpicNicks/N2Engine, workflow run 38454730232): asset https://github.com/EpicNicks/N2Engine/releases/download/physx-dependencies-windows-5.6.1/physx-windows-x86_64-5.6.1-slim.zip (536,854,385 bytes, sha256 27258b5ff6eae3e8e99243cca67bee6330d1bf48b41a51c4083466b8e2c91fa1) plus a .sha256 file (sha256 of that file b52dc24f3dc7bf53d4b90342c676a05afda17a47e8a34d5499a7b9c4a7c8df28). Layout: bin/win.x86_64.vc143.mt/{debug,release}/... extracting at external/PhysX/physx exactly like bin.zip, plus MANIFEST.txt and LICENSE* inside bin/win.x86_64.vc143.mt/ (so they stay git-ignored). Built from the original bin.zip (sha256 77eedc4ba568479ff8657c1a04d50c7ce2d2b2046ca277e7b77809776746d527, 1405731918 bytes; original build recipe unknown). The windows-physx-slim-build verify jobs already built and ran the PhysX tests (Debug and Release, PhysX ON) against exactly these files. LAUNCH ONLY AFTER particles-component-cpu (#125) has merged (it edits CMake files too) and re-read the then-current root CMakeLists.txt and cmake/PhysXLinuxHashes.cmake pattern first. Scope: (1) root CMakeLists.txt (~l.34-75): download the slim zip from the new release instead of bin.zip with the same pattern as the Linux hashes (cmake/PhysXLinuxHashes.cmake is the model: a new tracked cmake/PhysXWindowsHashes.cmake with the pinned version and sha256, SHA256 checked with file(SHA256) after download, a mismatch or failed download warns and turns PhysX OFF, a hash stamp so a changed pin re-downloads), keep the existing 'binaries already present' short circuit working for developers who still have the old bin.zip tree (the file names are the same). (2) tests.yml: the Windows legs' PhysX cache key must change when the pin changes (hashFiles of the hashes file, like the Linux legs). (3) docs/testing.html, docs/physics.html and THIRD_PARTY_NOTICES.md: the slim asset, its provenance (derived from bin.zip, original build recipe unknown), how to bump it. Acceptance: ALL Windows legs (Debug PhysX ON, Release PhysX ON, TLS leg) and the Linux/Docker legs stay green; the Windows legs must really use the slim files (the job logs show the new asset and hash).

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^docs/
- ^THIRD_PARTY_NOTICES\.md$
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green; Windows legs download the slim asset and verify its pinned hash
- A bad hash or failed download turns PhysX OFF with a warning
- Docs describe the asset, provenance and how to bump it
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
The old bin.zip stays published for older commits. Unverified: developer machines that still have the old extracted tree keep working via the short circuit.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs windows-physx-slim-enable` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-windows-physx-slim-enable` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/windows-physx-slim-enable/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-windows-physx-slim-enable --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/windows-physx-slim-enable/dev.json: {"pr":<number>,"branch":"feat-windows-physx-slim-enable","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-windows-physx-slim-enable` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
