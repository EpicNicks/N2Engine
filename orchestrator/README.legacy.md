# Orchestrator

Token-lean loop for the remaining N2Engine work. Agents pass files to each other, never conversation.

## Pieces
- `items/<name>.json` defines a work item: repo, branch, plan text, allowed paths, acceptance, and a `gate`.
  An item with a `gate` and `approved: false` is skipped (brief.mjs prints `{blocked}`). Fill in the plan, set `approved: true`.
- `lib/brief.mjs <name>` writes `ledger/<name>/brief-dev.md` and `brief-review.md` (the fixed rules block is identical in every brief).
- `lib/setup.mjs <name>` (run by the developer) creates the worktree from origin/master.
- `lib/ci-wait.mjs <name>` polls the Actions runs for the PR head (85 s) and prints only failing lines (max 25).
- `lib/ratify.mjs <name> [--merge]` is the scripted gate. It merges only if: no needsUser or deviations, the review is of the
  current head with no blocker/high, CI green on that head, branch current with master, every changed file matches
  `allowedPaths`, docs changed if required, no protocol.json change unless allowed, no token or attribution, and unverified
  items listed in the PR body. It adds the `## Review` section, merges, deletes the branch, cleans up the worktree.
- `run.workflow.js` is the Workflow script: Develop -> (CI wait || fresh reviewer) -> up to 2 fix rounds -> ratify.
  Script steps are run by Haiku agents that only execute one command and echo its JSON.
- `ledger/<name>/`: `dev.json`, `review.json`, `ci.json`, `ratify.json`, `state.json` (kept outside the repos so they add no CI churn).

## Run
1. `node lib/brief.mjs <name>` for each approved item.
2. Workflow `{scriptPath: "C:/Method Notes Obsidian/orchestrator/run.workflow.js", args: {items: [{name, reviewer}]}}`.
3. Each item returns one line: merged / held (reasons) / needs-user.

Self-test: `_selftest` points at merged frontend PR #24 (branch deleted, so ratify must hold).

## Resuming (2026-10-16)
The pipeline was paused by the user. `CONTINUE.md` is the entry point: standing rules, the full pipeline how-to, the exact state, the queue, the needs-user list and the resume prompt. Working copies of the helper scripts are in `tools/` (`orchestrate-items-workflow.js` = the Workflow script now in use instead of `run.workflow.js`, `accept.js`, `sethead.js`, `poll-template.sh`).
