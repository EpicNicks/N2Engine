# Saved plans and orchestrator

This directory preserves the N2Engine planning and orchestration files that
previously lived outside the repository at
`C:/Method Notes Obsidian/orchestrator`. The snapshot was imported on
2026-10-22 UTC (2026-10-21 in America/Toronto). It includes the full netcode
plan and the state left when Claude's session reached its usage limit.

## Start here

- [Netcode runtime design](proposals/netcode-runtime-design.md): the accepted
  design decisions and stage table, including B3, B4, B5 and B7.
- [Proposals](proposals/): the other saved design plans and verification lists.
- [Latest progress log](project-memory/pending-minor-defects.md): read the last
  entries first; these are newer than the dated handoff documents.
- [Latest saved handoff](CONTINUE-2026-10-22.md) and
  [older handoff](CONTINUE.md): historical workflow instructions and queue
  snapshots, whose status sections may be out of date.
- [Orchestration lessons](project-memory/orchestrator-lessons.md): operational
  context recovered from the session's project memory.

## Contents

| Path | Contents |
| --- | --- |
| `items/` | Stage briefs, scope, acceptance criteria and allowed paths |
| `ledger/` | Saved developer/reviewer briefs, commit references, reviews, CI results and decisions |
| `lib/` | Original Node scripts for briefs, worktrees, CI and merge gates |
| `tools/` | Original workflow scripts and item-maintenance helpers |
| `run.workflow.js` | Original workflow entry point |
| `project-memory/` | Relevant project plans, progress and operating notes recovered from local memory |
| [README.legacy.md](README.legacy.md) | Original orchestrator README |

## Status at import

The B5 controller work is committed and pushed on
`feat-net-b5-bodystate-controller` at `b79a314d`
([PR #223](https://github.com/EpicNicks/N2Engine/pull/223)). The first Jolt
determinism golden is committed and pushed on
`chore-physics-determinism-golden` at `b032835e`
([PR #225](https://github.com/EpicNicks/N2Engine/pull/225)). Neither was merged
by this import. B3 clock/interpolation, B4 input frames and B7 RPCs have saved
briefs and initialized worktrees, with no implementation changes at the time
of inspection. Verify GitHub before treating a saved CI or review result as
current.

## Using the saved scripts

This is a preservation snapshot, not a portability migration. The scripts
and briefs retain their original absolute Windows paths, sibling repository
locations and references to Claude's workflow runtime. Simply checking out
this directory does not configure or start the orchestrator.

Before running anything, adapt those paths and provide the expected workflow
runtime and a locally configured GitHub authentication wrapper. That wrapper,
credentials and raw conversation logs are not included. Some scripts push
branches, edit PRs or merge them when invoked; importing these files does not
invoke them. Historical approval records and agent instructions describe the
original sessions and do not grant approval for future work.

The original external directory was left in place. Further changes there do
not automatically update this checked-in snapshot.
