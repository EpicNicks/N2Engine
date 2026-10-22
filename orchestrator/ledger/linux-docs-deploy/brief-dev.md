# Task linux-docs-deploy: Linux host docs, systemd unit and Dockerfile
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-linux-docs-deploy.

## Issue / plan
Approved by the user 2026-10-10 (proposal: orchestrator/proposals/linux-host.md; answers: PhysX OFF on Linux for now with a later PhysX-for-Linux PR, x86_64 AND arm64 eventually, host stays loopback-bound behind an SSH tunnel/proxy, the ubuntu leg becomes a required check once green (the user sets branch protection), smoke test ported to one Python script for both OSes, install X11/Vulkan dev packages for now, Dockerfile AND systemd unit). Nothing can be built locally; CI is the build. Scope (PR D): docs/editor.html section on running the host on Linux (packages, build, loopback bind behind an SSH tunnel or reverse proxy, --token-env, --tls and the certificate directory, systemd stdin is /dev/null so never pass --exit-on-stdin-eof there, firewall guidance); deploy/n2editorhost.service (systemd, hardened: dedicated user or DynamicUser, StateDirectory for the TLS identity and project, NoNewPrivileges, token via EnvironmentFile or LoadCredential); deploy/Dockerfile (multi-stage, build with the apt list, minimal runtime image, non-root, volume for the project and the TLS identity, binds loopback by default with a documented override). Nothing here runs in CI: list as unverified the image build and the systemd unit on a real machine; if a docker build step can be added to CI cheaply, do it. Context update (2026-10-11): #113 #114 #115 #116 merged. Build recipe to document and put in the Dockerfile: Ubuntu 24.04, the apt packages from the tests.yml linux job, Clang 20 from apt.llvm.org with libstdc++ 14 via --gcc-install-dir (copy the exact steps from the `linux (Release, x86_64, Clang)` job; GCC-built hosts are unsupported until the sol2 same-signature-lambda bug is fixed), PhysX OFF on Linux for now (a separate PhysX-for-Linux item follows), arm64 builds scalar-only (no NEON). The Dockerfile must build on both x86_64 and arm64 (buildx multi-arch is fine, no CI publish needed). The host binary is build/bin/N2EditorHost; it binds 127.0.0.1 by default (check HostOptions for the bind option; if none exists, document that and do NOT add one in this item: report it as needsUser if the Docker use case needs one, since a container needs 0.0.0.0 inside plus a published loopback port). User data/TLS identity: $XDG_DATA_HOME/n2engine or $HOME/.local/share/n2engine, no HOME means --tls refuses (PR #114): the systemd unit and the Dockerfile must set HOME or XDG_DATA_HOME/StateDirectory. Also fix the open low from linux-arm64: docs/testing.html ~line 290 says the TLS smoke test runs only on the Windows TLS ON leg and the Linux Clang leg; the arm64 Clang leg runs it too. Mention the required check name `linux (Release, x86_64, Clang)` where CI is described.

## Allowed paths (regex, repo-relative)
- ^docs/
- ^deploy/
- ^README\.md$
- ^\.github/workflows/tests\.yml$

## Acceptance
- Docs, unit and Dockerfile present and consistent with the host options
- All CI jobs green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Draft. Hardening for exposed hosts (per-user tokens, rate limiting, read-only mode) stays deferred and is mentioned as such.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs linux-docs-deploy` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-linux-docs-deploy` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/linux-docs-deploy/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-linux-docs-deploy --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/linux-docs-deploy/dev.json: {"pr":<number>,"branch":"feat-linux-docs-deploy","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-linux-docs-deploy` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
