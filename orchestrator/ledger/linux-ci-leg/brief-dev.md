# Task linux-ci-leg: Linux build leg: ubuntu CI job, winsock shim in the tests, first GCC/Clang compile fixes
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-linux-ci-leg.

## Issue / plan
Approved by the user 2026-10-10 (proposal: orchestrator/proposals/linux-host.md; answers: PhysX OFF on Linux for now with a later PhysX-for-Linux PR, x86_64 AND arm64 eventually, host stays loopback-bound behind an SSH tunnel/proxy, the ubuntu leg becomes a required check once green (the user sets branch protection), smoke test ported to one Python script for both OSes, install X11/Vulkan dev packages for now, Dockerfile AND systemd unit). Nothing can be built locally; CI is the build. Scope (PR A): (1) add an ubuntu-24.04 job to .github/workflows/tests.yml (name it `linux (Release, x86_64)`): apt install libvulkan-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev python3 and a GCC 14 or Clang 18 toolchain (pick what ubuntu-24.04 provides; state the choice); cmake -S . -B build -DN2ENGINE_USE_PHYSX=OFF -DN2ENGINE_EDITOR_TLS=ON -DN2ENGINE_TEXT_FREETYPE=ON -DN2ENGINE_MODEL_UFBX=ON; build the host and test targets (not necessarily test_project); ctest --output-on-failure with ALSOFT_DRIVERS=null. The host smoke test is NOT part of this PR (item linux-smoke-test). (2) a small shared tests header that gives the tests the socket names on both OSes (replaces the direct winsock use in tests/editor-server/EditorServerHardeningTests.cpp, HostOptionsTests.cpp, project/EditorProjectTests.cpp and any other tests/ file that fails on Linux). (3) the minimal source fixes the first GCC/Clang compile of engine/renderer/editor-server needs (missing includes, narrowing, MSVC-isms); no behaviour change on Windows. Expect several CI rounds: iterate until the ubuntu leg is green and the 3 Windows legs and protocol job stay green. Anything that cannot be fixed without a design decision (for example a renderer or GLFW refactor) is reported as needsUser, not worked around. Do not change the runtime behaviour of the host (SIGPIPE is item linux-runtime-fixes).

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^renderer/
- ^math/
- ^text/
- ^assetimport/
- ^tests/
- ^lua_project/
- ^test_project/
- CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green including the new ubuntu leg (ctest passes there)
- No behaviour change on Windows
- The first-compile fixes are listed in the PR body

## Notes
First compile of ~7000 server lines and the engine on GCC/Clang: the number of fix rounds is unknown. Give the ubuntu job a generous timeout (90 min like windows). If configure needs a package not in the apt list, add it and say so. Unverified: a real Linux host run (the smoke test comes later).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs linux-ci-leg` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-linux-ci-leg` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/linux-ci-leg/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-linux-ci-leg --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/linux-ci-leg/dev.json: {"pr":<number>,"branch":"feat-linux-ci-leg","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-linux-ci-leg` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
