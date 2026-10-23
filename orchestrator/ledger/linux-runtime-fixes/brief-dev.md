# Task linux-runtime-fixes: Linux runtime fixes: no SIGPIPE kill, UserDataBase without $HOME
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-linux-runtime-fixes.

## Issue / plan
Approved by the user 2026-10-10 (proposal: orchestrator/proposals/linux-host.md; answers: PhysX OFF on Linux for now with a later PhysX-for-Linux PR, x86_64 AND arm64 eventually, host stays loopback-bound behind an SSH tunnel/proxy, the ubuntu leg becomes a required check once green (the user sets branch protection), smoke test ported to one Python script for both OSes, install X11/Vulkan dev packages for now, Dockerfile AND systemd unit). Nothing can be built locally; CI is the build. Scope (PR B, unblocked now that #112 is merged): (1) EditorServer: pass MSG_NOSIGNAL on every send on non-Windows and call signal(SIGPIPE, SIG_IGN) once at host start (covers the Mbed TLS BIO writes too); a client that drops mid-frame must never kill the host. (2) Verify the non-Windows branch of UserDataBase() in engine/src/ProjectFile.cpp: XDG_DATA_HOME then $HOME/.local/share, and a defined, documented behaviour (a clear error or a fallback; pick the safest and say why) when neither is set (systemd, docker). The TLS identity dir <UserDataBase()>/editor-tls must stay 0700 and the key 0600. (3) Tests meaningful on both OSes: a client that closes with a pending large send does not terminate the test process, and UserDataBase with HOME unset (platform-neutral env helper in the test). The code cannot be run on Linux until item linux-ci-leg merges, so write it to compile on MSVC today and on GCC/Clang with no Windows-only names; guard POSIX-only calls with #ifndef _WIN32. Update docs/logging-and-editor.html (or editor.html) with the Linux behaviour.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- All existing Tests-workflow jobs green
- The new tests run on the Windows legs
- docs state the SIGPIPE and no-HOME behaviour
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive (key directory permissions, behaviour without HOME). Unverified: running on a real Linux host (until the ubuntu leg exists).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs linux-runtime-fixes` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-linux-runtime-fixes` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/linux-runtime-fixes/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-linux-runtime-fixes --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/linux-runtime-fixes/dev.json: {"pr":<number>,"branch":"feat-linux-runtime-fixes","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-linux-runtime-fixes` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
