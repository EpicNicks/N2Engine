# Task linux-smoke-test: One cross-platform Python smoke test for the editor host, run on Windows and Linux
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-linux-smoke-test.

## Issue / plan
Approved by the user 2026-10-10 (proposal: orchestrator/proposals/linux-host.md; answers: PhysX OFF on Linux for now with a later PhysX-for-Linux PR, x86_64 AND arm64 eventually, host stays loopback-bound behind an SSH tunnel/proxy, the ubuntu leg becomes a required check once green (the user sets branch protection), smoke test ported to one Python script for both OSes, install X11/Vulkan dev packages for now, Dockerfile AND systemd unit). Nothing can be built locally; CI is the build. Scope (PR C): port the long pwsh host smoke step in .github/workflows/tests.yml (docs/testing.html describes it) to one Python 3 script (tests/smoke/editor_host_smoke.py, standard library only) that runs on both OSes: create a project, start N2EditorHost, parse the ready line with the documented regex, open a TCP session with Hello and the token (via --token-env), exercise what the pwsh step exercises today, stdin-EOF shutdown, and for the TLS leg: TLS handshake, fingerprint equals the ready line, certificate reuse across restarts, key file mode 0600 on POSIX. Windows jobs and the ubuntu job both call it; keep the pwsh step only if something in it cannot be ported (say so). Update docs/testing.html. Specifics (re-read against master after #113/#114): the two pwsh steps in tests.yml are `Editor host smoke test` (~500 lines: create project, ready line, session, scene save/reopen, hierarchy, duplicate, GetEntity, GetProjectInfo user:// folder, and more) and `Editor host TLS smoke test`; port BOTH, step by step with the same assertions, into one or two Python modules under tests/smoke/ (a shared frame codec module plus the scenarios). Keep the Windows pwsh steps in place until the Python version is green on Windows, then switch the Windows legs to Python and delete the pwsh steps only if every assertion is covered (list the mapping in the PR body). `Play child smoke test` (tests/editor-server/PlaySmokeTest.ps1, needs a Windows renderer) stays pwsh and is out of scope. The Linux Clang job (named `linux (Release, x86_64, Clang)`, the required check, do not rename it) gains the Python smoke steps; the GCC leg does not. The user:// folder assertion must accept the Linux path ($XDG_DATA_HOME/n2engine or $HOME/.local/share/n2engine, see PR #114). Host binary path on Linux is build/bin/N2EditorHost.

## Allowed paths (regex, repo-relative)
- ^tests/
- ^docs/testing.html$
- ^.github/workflows/tests.yml$

## Acceptance
- Windows legs and the ubuntu leg all run the Python script and pass
- docs/testing.html describes the script
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Draft: re-read the pwsh step and the merged ubuntu leg before approving. Unverified: anything Windows-ACL specific.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs linux-smoke-test` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-linux-smoke-test` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/linux-smoke-test/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-linux-smoke-test --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/linux-smoke-test/dev.json: {"pr":<number>,"branch":"feat-linux-smoke-test","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-linux-smoke-test` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
