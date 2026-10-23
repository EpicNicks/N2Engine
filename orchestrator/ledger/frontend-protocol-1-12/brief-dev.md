# Task frontend-protocol-1-12: Sync to protocol 1.12.0: user field in the connect dialog, user/role shown, viewer role disables writes and play
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-protocol-1-12-users.

## Issue / plan
Engine PR #130 (hardening PR B, merged) shipped protocol 1.12.0: Hello gained a trailing optional `user` string, ServerInfo gained trailing `user` and `role` (admin|editor|viewer; a host without a tokens file answers user '' and role 'admin'; --token-env answers user 'token', role 'admin'), and the host denies commands with ErrorCoded `forbidden` (the 1.11.0 mechanism the frontend already handles). Read the engine repo (C:/Method Notes Obsidian/N2Engine, READ-ONLY) docs/editor.html and the protocol.json history for the exact semantics first. Scope: (1) bump engine-ref.txt to the current engine master commit and run `npm run sync-protocol` so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, never hand-edited (the CI 'Generated protocol matches the pinned engine' check must stay green). (2) engine-client: send the optional user in Hello when the connect options carry one (leave it absent otherwise, which older hosts accept), expose serverInfo.user/role. (3) Connect UI: an optional 'User' field next to the token in the direct-remote form and the other connect paths that take a token (welcome.tsx DirectForm and the ssh/remote forms if they take a token); remembered per recent remote like the other non-secret fields (the TOKEN itself must never be persisted or logged, as today); keep the TLS pinning/TOFU behaviour and its tests unchanged. (4) Show the signed-in user and role in the editor chrome next to the read-only badge from 1.11.0 (e.g. 'alice (editor)'). (5) A `viewer` role behaves like the existing read-only mode for the write guard (write-guard.ts: all write affordances disabled including Play, via the same central canWrite path; a viewer is read-only AND cannot play); an `editor` can write but Shutdown/Admin-class actions the UI exposes (if any, e.g. stopping the host) are disabled with a tooltip; a `forbidden` ErrorCoded denial shows the host's message in the existing banner and, for a forbidden code, does NOT latch the whole UI read-only (only readOnly does), but the denied edit is reverted as today. (6) Tests: codec and golden-vector tests pass after the sync; unit tests for Hello with and without user, ServerInfo decoding of an old (1.11) and new payload; DOM tests for the user field, the user/role badge, and a viewer's disabled controls (use the existing harness in src/test-dom and src/test). (7) README: protocol version, the user field, roles. Run npm run typecheck, build and npm test locally; the repo has CI: open a PR and wait for its checks. Do not change the engine repo. Security: never log or persist the token; the user name is not secret. If a UI path cannot be gated without a design decision, stop and report needsUser.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt (CI check passes)
- Optional user sent in Hello; user/role shown
- viewer role: all writes and Play disabled; editor denial handled as a banner message
- typecheck, build, tests and frontend CI green

## Notes
Reviewer: check the token is never persisted/logged, the user field round-trips only when set, viewer/readOnly share the central write guard, and the old-host (1.11 ServerInfo without user/role) path. Unverified: the Electron UI against a live host with a tokens file.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-protocol-1-12` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-12/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-protocol-1-12-users --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-12/dev.json: {"pr":<number>,"branch":"feat-protocol-1-12-users","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-protocol-1-12-users` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
