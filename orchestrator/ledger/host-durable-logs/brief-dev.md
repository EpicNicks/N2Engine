# Task host-durable-logs: Editor host: durable log output (stderr sink, optional log file) so audit, rate-limit and reload lines survive in journald and docker logs
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-host-durable-logs.

## Issue / plan
Follow-up approved by the user 2026-10-14 after hardening PR D (#133): docs/editor.html and docs/logging-and-editor.html now state that a RELEASE editor host prints no log lines (the logging system has no console subscriber in release, so the audit lines (user=<u> role=<r> peer=<addr>, 'audit:' per mutating command), rate-limit ban/unban lines and token-reload lines are only visible through the editor protocol's PollEvents, never in the journal or `docker logs`). Fix it. First read: docs/logging-and-editor.html, the engine's logging code (grep for the Log/Logger subscriber/sink interfaces under engine/ and editor-server/), editor-server/src/Host.cpp and HostOptions.cpp (how the host starts, its flags, how Debug builds print), and the 'Not covered / no durable audit trail' wording PR D added in docs/editor.html. Scope: (1) the editor host (N2EditorHost, Host.cpp) installs a log sink that writes one line per log event to STDERR with a timestamp and level (works under systemd journald and docker logs without extra config), enabled by default for the host binary in ALL build types, with a minimum level option (e.g. `--log-level debug|info|warn|error`, default info) so polled-command noise stays out (polled commands are already silent; keep Debug-level lines off by default). (2) Optional `--log-file <path>`: append the same lines to a file (opened append-only; on POSIX create it with mode 0600 so audit lines are not world readable; refuse an obviously unwritable path at start with a clear error; no rotation in this PR, document that logrotate with copytruncate or a restart is needed, or reopen on SIGUSR1 ONLY if trivial and tested). (3) Make absolutely sure no token material, token hash or Hello token ever reaches any sink (re-audit every log call in editor-server/ and the new sink: only user names that passed IsValidTokenUserName, SanitizeForLog on client-supplied text) and add a test that drives a failed Hello with a recognisable fake token and asserts the token text and its hash never appear in captured sink output. (4) Tests that run on every leg: sink formatting (timestamp injectable or pattern-matched, level filter), the file sink (temp dir, append, 0600 on POSIX, unwritable path error), and an end-to-end socket test that a refused Hello, a mutating command (audit line) and a rate-limit ban (injected clock/address provider from PR C) produce the expected lines in a captured sink; the sink must not change behaviour of the Debug console output or of PollEvents (the editor still receives the same events). (5) Docs: remove or rewrite the 'no durable audit trail' limitation in docs/editor.html, docs/logging-and-editor.html and docs/testing.html, document the flags, the line format, and the journald/docker examples (`journalctl -u n2editorhost`, `docker logs`); update the systemd units/compose only where a flag is needed (deploy/ is in scope, keep edits minimal). No protocol change. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`); the docker leg's smoke step can assert a line shows up in `docker logs` if straightforward (tests/smoke, tests.yml in scope), otherwise list it as unverified.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/
- ^deploy/
- ^Dockerfile$
- ^\.github/workflows/tests\.yml$
- ^cmake/
- ^CMakeLists\.txt$

## Acceptance
- All Tests-workflow jobs green
- Release host writes audit, rate-limit and reload lines to stderr (and optionally a 0600 log file)
- Test proves token text and hash never appear in sink output
- Docs updated: no 'no durable audit trail' limitation left; flags documented
- No protocol change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-adjacent: reviewer checks no token material in any log path, the file mode, that stderr logging cannot block or crash the host (closed stderr, EPIPE/SIGPIPE: the host already ignores SIGPIPE), no behaviour change to PollEvents/Debug output, and log volume (no per-poll lines). Unverified: journald/docker logs on real systems, logrotate behaviour.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs host-durable-logs` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-host-durable-logs` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/host-durable-logs/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-host-durable-logs --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/host-durable-logs/dev.json: {"pr":<number>,"branch":"feat-host-durable-logs","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-host-durable-logs` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
