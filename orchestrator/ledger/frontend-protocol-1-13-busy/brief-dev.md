# Task frontend-protocol-1-13-busy: Frontend: sync protocol 1.13.0, map the host's `busy` error to a clear message with retry-with-backoff, show 'taken over by another session' on takeover/preemption
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-protocol-1-13-busy.

## Issue / plan
Engine PR #177 (pre-auth slots S-4, merged) shipped protocol 1.13.0 (additive minor): a new `busy` entry in the errorCodes, sent as ErrorCoded `busy` when the connecting client's Hello asked for errorCodes (the frontend already asks: see how 1.11.0/1.12.0 coded errors such as `forbidden`/`readOnly` are handled), otherwise a plain Error. Host behaviour you must understand first (read the engine repo C:/Method Notes Obsidian/N2Engine READ-ONLY: docs/editor.html and docs/logging-and-editor.html sections on connection limits, pre-auth slots, Busy, preemption and takeover; editor-server/protocol/protocol.json; the merged PRs #176 and #177): the host serves ONE editor session at a time; a second client whose token verifies gets Busy ('The host is serving another session (a <role> session); try again later', after up to 1 s grace) unless it is the SAME user and role (takeover) or a STRICTLY HIGHER role (preemption: admin > editor > viewer), in which case the OLD session is closed by the host and the newcomer is served; a no-token host: the newest connection takes over; excess or silent pre-auth connections are closed at once without a message. Scope: (1) bump engine-ref.txt to the current engine master commit and run `npm run sync-protocol` so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, NEVER hand-edited (the CI check 'Generated protocol matches the pinned engine' must stay green). (2) engine-client: surface a Hello answered with ErrorCoded `busy` as a typed error (e.g. `HostBusyError` with the holder's role parsed from the text ONLY if present: treat the text as display-only, never parse behaviour from it; the code is the contract), distinct from authentication errors; ErrorCoded `busy` must NOT be treated like a bad token or a TLS pin mismatch (no 'forget this host', no credential prompt). (3) Connect UI: on `busy`, show a clear non-alarming message ('The host is busy with another session (role). It will retry.' or similar) with automatic retry using capped exponential backoff with jitter (e.g. 1 s, 2 s, 4 s ... capped at 15 s, give up after about 2 minutes with a Retry button), a Cancel button, and never retry on authentication/pin errors; the retry must reuse the same connect options and must not leak the token to logs or storage (the token is never persisted or logged, as today). (4) When an ESTABLISHED session is closed by the host because another session took over or preempted it, the connection simply ends: detect this case as best the protocol allows (the host closes the socket after normal cleanup: check whether the engine sends an Error/notice frame before closing in the merged S-4 code; if it does, show 'This session was taken over by another session' using it; if it does not, show the generic 'The host closed the connection' with a hint that another client may have taken over, and do NOT auto-reconnect when the user is the same one (that would fight the other session): offer a Reconnect button instead; auto-reconnect behaviour for network drops stays as it is). (5) Tests: engine-client unit tests with the golden vectors for ErrorCoded busy; DOM tests (the repo has jsdom DOM tests from #27/#29/#30: follow them) for the busy message, the retry/backoff schedule (inject a clock/timers), cancel, give-up, and that pin/auth errors do not retry; the takeover-closed message; update README/docs strings if the repo documents connection errors. Keep the TLS pinning/TOFU behaviour and tests unchanged. Frontend CI runs typecheck, build and tests (run them locally: `npm run typecheck`, `npm test`, the build script, plus the DOM tests) and there is a live-host verification the user does later: list in the PR what needs a live host (issue #149 in the engine repo tracks the live verification). Engine repo is NOT touched.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt (CI check passes)
- ErrorCoded busy is a typed error distinct from auth/pin errors; clear message with capped exponential backoff retry, cancel and give-up
- A host-closed session shows a takeover/closed message without auto-reconnect fighting the other session
- typecheck, build, tests and frontend CI green; token never persisted or logged

## Notes
Reviewer: no hand edits to generated files (sync-protocol output only), the busy path cannot leak or persist the token, retry stops on auth/pin errors, backoff capped and cancellable with timers cleaned up on unmount/cancel (no leaked timers), the takeover message does not auto-reconnect and fight the other session, role text from the host is display-only and sanitised (no HTML injection: React text only), tests meaningful. Unverified: anything needing a live host.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-protocol-1-13-busy` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-13-busy/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-protocol-1-13-busy --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-13-busy/dev.json: {"pr":<number>,"branch":"feat-protocol-1-13-busy","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-protocol-1-13-busy` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
