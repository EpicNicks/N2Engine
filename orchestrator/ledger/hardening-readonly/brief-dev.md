# Task hardening-readonly: Exposed-host hardening PR A: default-deny read-command classification, --read-only host mode, denial error code (protocol 1.11.0)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-hardening-readonly.

## Issue / plan
Approved by the user 2026-10-13 (design and decisions: C:/Method Notes Obsidian/orchestrator/proposals/exposed-host-hardening.md, sections 1, 2c, 3, 4 and the Decisions block; read it first). Decisions that apply: viewers are strictly read-only and may NOT enter play mode; a denial gets a new ERROR CODE (not a message prefix), which means a minor protocol bump 1.10.0 -> 1.11.0; the pre-auth accept thread and proxy support are out of scope. Scope (PR A, useful alone, no auth change): (1) editor-server/src/EditorServer.cpp: add `IsReadCommand`, an ALLOW list with default deny (seed from IsPolledCommand ~l.1971, mostly reads, plus GetCurrentScene, GetOpenScene, GetProjectInfo, GetPlayState; review every protocol.json command and classify it explicitly as read / write / admin; commands that can execute scripts or change anything, including SendInput, WritePlaySnapshot, EnterPlayMode/play-mode commands, Shutdown, RescanAssets, Undo/Redo, Save*, import settings, are NOT read). Do not reuse IsEditOnlyCommand (it only lists 13 file-writing commands and runs only in play mode). (2) a host option `--read-only` (HostOptions.cpp + docs): the gate sits beside the existing _playMode check (~l.2114) and refuses every non-read command with the denial error; it works with the legacy --token-env token and with no token. (3) the denial error code: add an optional trailing `code` to the Error message (or the closest equivalent in protocol.json; find how Error is declared, keep old clients working through a trailing field), define stable codes (`readOnly`, `forbidden`), bump protocol minor to 1.11.0, regenerate every generated client and the golden vectors with the generators in editor-server/protocol/generators (cpp, csharp, typescript, test_vectors), commit the regenerated files; the `protocol` CI job must stay green. ServerInfo gains NOTHING in this PR except what is needed to say the host is read-only: add a trailing `readOnly` bool to ServerInfo only if cheap and backwards compatible, otherwise leave it for the per-user-tokens PR and say so. (4) Tests (socket/unit, run on every leg incl. Linux): an EXHAUSTIVE classification test that iterates every command id in protocol.json and requires an explicit class so a new command cannot ship unclassified (fail with the command name); with --read-only every write command is refused with the right code and the connection stays usable, every read command still works, Shutdown is refused; Hello/ping/health keep working; play-mode behaviour unchanged. (5) Docs: docs/editor.html and docs/logging-and-editor.html (the --read-only option, the classification, the denial code, the protocol 1.11.0 entry in the protocol history/changelog place the docs use), deploy docs mention if relevant. Do not touch the frontend repo (a later item syncs it). DECISION UPDATE (user, 2026-10-13, after the first attempt stopped on this): the Error payload is raw UTF-8 message bytes with no length prefix, so DO NOT append a trailing code to Error. Use a NEW response message `ErrorCoded` (message + code) and keep Error unchanged. A host sends ErrorCoded only to clients that declare support in Hello through a new TRAILING Hello capability flag (add it as an optional trailing field with a default of false so old clients, which never send it, keep working); a client without the flag keeps getting the plain Error with a readable message such as `read-only host: <command> is not allowed` so older clients still see why. Add the new response id in protocol.json (next free id in the right range), bump the minor protocol version to 1.11.0, regenerate all generated clients and golden vectors, document the capability negotiation and the codes (readOnly, forbidden) in docs/logging-and-editor.html, and test both paths (new client gets ErrorCoded with the right code; legacy client gets Error). A previous attempt left a malformed ledger/hardening-readonly/dev.json (a stray backslash sequence): overwrite it with valid JSON when you finish. If a worktree for this item already exists (ledger/hardening-readonly/state.json), reuse it instead of creating another.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^tests/
- ^docs/
- ^deploy/
- ^README\.md$

## Acceptance
- All Tests-workflow jobs green on every leg (protocol job: generated clients and golden vectors up to date)
- The exhaustive classification test fails for an unclassified command (show by inspection) and passes for all current commands
- --read-only refuses all writes with the denial code and keeps reads working
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive: the reviewer checks default-deny really holds (an unknown id is refused), that nothing that executes Lua/engine code or mutates state is in the read list, play-mode/edit-only behaviour unchanged, backwards compatibility of the Error message change for old clients (trailing field), and that every generated file matches the generators. Unverified: the frontend showing the denial (a later frontend item), a real exposed host.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs hardening-readonly` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-hardening-readonly` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/hardening-readonly/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-hardening-readonly --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/hardening-readonly/dev.json: {"pr":<number>,"branch":"feat-hardening-readonly","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-hardening-readonly` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
