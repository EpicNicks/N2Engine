# Next-work plan (written 2026-10-31, when the user stopped the session for a handoff)

Read orchestrator/CONTINUE.md first (standing rules, pipeline, merge authorization), then this plan. This file is the sequenced plan for the work started on 2026-10-30/31; the progress log (orchestrator/project-memory/pending-minor-defects.md, last entry 2026-10-31) records what merged and why.

## 1. Decisions the user made (2026-10-31; do not reopen)

- **Stable UUIDs.** An asset's UUID stops being derived from its path (today `ResourceUUID::FromPath`, engine/src/io/ResourceUUID.cpp; `ResourceLoader.cpp` about line 329 regenerates any `.meta` whose uuid differs). It is stored in the asset's `.meta` (in `.import/`) and a move carries the `.meta`. The user chose this over the design's recommended redirect aliases (`movedFrom`). New assets get the path hash of their creation path, then it is stored: same value as today, so no existing project changes.
- **Asset move rules (accepted as written in asset-move.md):** multi-move is all-or-nothing with rollback; a name collision is refused, compared case-insensitively on every platform; moving the open scene, or a folder that holds it, is refused in v1; `LuaComponent::RetargetScript` so running scripts keep state; the `n2_move_asset` bridge tool comes LAST (frontend repo); moves into or out of `res://editor` keep today's parity and a separate item is filed.
- **Prefab references (orchestrator/proposals/prefab-references.md, PR #285), all accepted:** `Prefab.Instantiate` with no parent adds to the scene root; a NetworkObject prefab spawned from a script during a session raises an error in v1 (replication by UUID is a follow-up); external references nulled with one warning per prefab; the Prefab field has no default; Prefab is not available to editor extensions; every package must ship the UUIDs (`.import` or a generated uuid index), which gates the web build; protocol 1.25.0 then 1.26.0.
- **Prefab stage (prefab-stage.md, PR #285), all accepted:** BOTH modes in the first release (Isolated: the prefab alone in an empty scene; In Scene / context: shown inside the open scene with everything else dimmed and not editable), switchable in the stage toolbar; the stage swaps the loaded scene and parks the open one; the file's UUIDs are kept; root transform locked in Context; play and extension writes refused while a stage is open; external references cleared on open and Save with a warning; nested prefabs edited as plain objects; a deleted prefab refuses Save (no Save As); no stage autosave in v1; dimming via a per-draw `RenderState.dim` in all four renderer backends; protocol 1.26.0.
- **#281 deviations accepted** (merged): the rollback client is a LockstepRuntime in rollback mode; the snapshot blob travels in `WorldSnapshot::physics`.
- A prefab dropped on a GameObject (entity reference) field is refused with that field's name; instantiating a prefab into a field is NOT done (a possible follow-up for the user to decide).

## 2. Sequence (protocol minors are strictly serial; only ONE agent may compile C++ at a time)

**Step A: asset move with stable UUIDs (protocol 1.24.0).**
- A1 docs: PR #284 (branch `docs-asset-move-design`, orchestrator/proposals/asset-move.md). The stable-UUID revision and the packaging section ("ship .import") are already pushed (commits 5938849b, 0e496e75). Check `gh pr checks 284`, then merge it (docs-only).
- A2 code, branch `feat-asset-move` (NOT on origin): the previous agent was STOPPED mid-work. Its worktree is `E:\Code\Projects\N2Engine\.claude\worktrees\agent-a490c5b60b6499d93`; one local WIP commit 4e8ba0dd holds five edited files (engine/include/engine/io/AssetMetadata.hpp, ResourceLoader.hpp, ResourceUUID.hpp, engine/src/io/AssetMetadata.cpp, ResourceUUID.cpp, +62 lines). It is unreviewed and was never built. Do not delete that worktree before the work is pushed (or move the branch). Continue from it or restart from master.
- A2 scope: stable-UUID loader change; duplicate-UUID detection (a file copied with its `.meta`: keep the first, regenerate the later with a warning); a lost `.meta` re-derives from the current path with a warning; `MoveAsset` 0xAF (a list of {from,to}), response `AssetsMoved`, `assetsChanged` gets a `moved` key (moved paths also stay in added/removed so old clients keep working), Write class, audit line; `LuaComponent::RetargetScript`; GATING: every package (web, Docker, Windows zips, exports) must carry the UUIDs: the web package today packs only `assets/` and `project.n2proj`, NOT `.import/`; ship `.import` or a generated path-to-uuid index the runtime loads when there is no `.import` (grep every use of `FromPath`, packaging in tools/ and CMake install rules, docs/web-build.html, determinism and golden tests that may hash UUIDs); generated clients and golden vectors (the CI `protocol` job); tests, editor_host_smoke additions, docs/editor.html and other pages in the SAME PR. If a question could break existing projects or packages: STOP and report.
- A3 frontend (after A2 merges): drag asset rows (Ctrl/Shift multi-select already exists) onto folder rows or the root, invalid-target feedback; then the `n2_move_asset` bridge tool, last.

**Step B: prefab references (protocol 1.25.0), after A2 merges.** Design in PR #285 (orchestrator/proposals/prefab-references.md): a `prefab = { type = "Prefab" }` Lua field (no asset-typed Lua fields exist today: this adds the first), `prefab:Instantiate(parent?, position?, rotation?)` and `Prefab.Instantiate(pathOrUuid, ...)` mirroring `Model.Instantiate` (engine/src/scripting/bindings/LuaComponentBindings.cpp about line 917); frontend field accepting only `.prefab`.

**Step C: prefab stage (protocol 1.26.0).** Design in PR #285 (prefab-stage.md). Order: engine + protocol (stage open/save/close commands, state and events), then the renderer `RenderState.dim` hook in OpenGL, Vulkan, WebGPU and the software backend, then the frontend (double-click a `.prefab` in the Assets panel, breadcrumb, Isolated/In Scene toggle, Save/Close with an unsaved-changes prompt). The biggest item; do not start it before B.

**Frontend, independent of the engine steps:** PR #69 (below) waits for the user's live check, then merge. Remaining frontend candidates: FE-S4, FE-S5 (needs EN5 host stats), the PickEntity hit point for a viewport prefab drop (HELD: needs a protocol minor, serial after 1.26.0).

## 3. Open PRs at handoff

| PR | What | State |
| --- | --- | --- |
| engine #284 | asset-move design (stable UUIDs, packaging) | docs-only; checks may still be running; merge when green |
| engine #285 | prefab references + prefab stage designs | docs-only; checks were pending; merge when green (touches only the two proposal files) |
| frontend #69 | inspector: lock button, script drop (one Lua component per script, one edit group), select on click not on drag start, per-field drop checks (GameObject/component fields refuse an asset with their own name) | CI green; HELD for the user's live Electron check (14-item checklist is a PR comment) |

Merged on 2026-10-30/31 are in the progress log: engine #279-#283, #281; frontend #66, #68.

## 4. Rules for the next agent (all learned the hard way this session)

- **Compilation:** the user needs the PC usable. One compile at a time, only the targets needed (never ALL_BUILD), a build directory OUTSIDE the repo, `(Get-Process -Id $PID).PriorityClass='BelowNormal'` before building, `--parallel 6` at most (the machine has 16 logical cores), check `Get-Process cl` afterwards, delete the build directory when done, E: has about 30 GB free. The default Windows host build has no `--tls`; the mcp smoke needs `N2ENGINE_EDITOR_TLS=ON` (a ready build is in `E:\Code\Projects\N2Engine-build-tls`, PhysX off; delete it when no longer used).
- **Frontend bridge tests:** rebuild `dist/n2-mcp.mjs` (`npm ci`, `npm run build:mcp`) before testing a bridge; a stale bundle announces an old protocol and the yield step fails. `npm test` in the frontend stops at one known failing test ('a frame assembled from several chunks is posted without a copy' in engine-ipc.test, a local Node 26.7 artifact, also fails on master): run `node --test "dist/test-dom/**/*.test.js"` and the mcp tests separately.
- **Line endings:** keep each file's endings (master: LF for most frontend .ts/.tsx, CRLF for the frontend README.md and some tests). A tool that rewrites a whole file as CRLF makes a 2600-line diff and conflicts: run `git diff --stat origin/master` before pushing.
- **Worktrees:** never create junctions; before `git worktree remove -f -f` check for a junction (see the 2026-10-30 incident in the progress log).
- **Merging:** docs-only PRs may be merged when CI is green. Code PRs need the user's go; deviations need the user's acceptance (AskUserQuestion, short, with a recommendation). Anything needing the user's hardware (live Electron, GPU) is HELD. No Claude attribution lines anywhere.
- **Docs pages (docs/*.html):** update in the same PR as the behaviour; verify against code. The page footers ('Describes master through PR #N') are NOT kept current and a blanket bump is wrong (an audit on 2026-10-31 did not read every page's prose).
- **Subagent reports can be stale:** check the ledger's LAST entry, the real PR head and `gh pr checks`; an agent once listed lows that were already fixed and claimed #269 had merged snapshot support (it was #281).

## 5. For the user (reminders)

- Live Electron check of frontend #69, then say so and it is merged.
- Frontend: the user's test editor may still be running from `E:\Code\Projects\N2EngineWebFrontend-livetest` (an older build with #66 + #68); delete that worktree, the project copy `E:\Code\Projects\N2Engine-livetest-project` and, when no longer needed, `E:\Code\Projects\N2Engine-build-tls`.
- Agent worktrees under `.claude/worktrees` (and the frontend worktrees `-fix-fe-lows`, `-multiprefab`, `-livetest`, `-inspector`) can be removed once their PRs are merged: check each for a junction first. Keep `agent-a490c5b60b6499d93` until `feat-asset-move` is pushed.
- Still the user's from earlier: live runs of the MCP checklist items that need a person (interactive Claude Code prompts, Claude Desktop, Docker `--connect`, a real editor taking the host, `--electron` live), a Blender FBX for #62, branch protection requiring `linux (Release, x86_64, Clang)`, whether Docker images or Windows zips are distributed, the B2 refused-value ack and skipped-layout ack, the B11 nudge on unopened scenes, the B5 kerb difference.
