---
name: editor-plan-issue6
description: "N2Engine #6 Editor/Project plan: user APPROVED all 20 decisions (with review changes) on 2026-10-06; phasing E0-E9 engine, F1-F7 frontend"
metadata:
  type: project
---

The #6 (Editor/Project system) proposal and its independent review were written on 2026-10-06. They are saved locally (excluded from git via `.git/info/exclude`; never commit them) at:
- `C:\Method Notes Obsidian\N2Engine\.claude\proposals\issue6-proposal.md`
- `C:\Method Notes Obsidian\N2Engine\.claude\proposals\issue6-review.md` (its "Decisions (proposed)" section and the revised phasing E0–E9 / F1–F7)

**Status:** E1b #73 = PR #88, E2 #74 = PR #89 (2026-10-07), E3 #75 = PR #90 (2026-10-08) merged; frontend PR #10 (codecs) and #11 (F2) merged. Next: E4 #76. The user APPROVED all 20 decisions with the review's changes and extras A-G. Posted on #6 (proposal in 2 comments + Decisions + split list). Sub-issues: engine #69-#71 (E0), #72 E1a, #73 E1b, #74 E2, #75 E3, #76 E4, #77 E5, #78 E6, #79 E7a, #80 E7b, #81 E8, #82 E9; frontend repo #2-#8 = F1-F7. E0 DONE 2026-10-06 (PRs #83, #84, #85 merged). E1a #72 merged as PR #87; frontend F1 merged as frontend PR #9. Next: E1b #73, E2 #74; frontend adopts generated codecs. The approved changes include:
- scenes reuse the existing `Scene` asset type;
- Electron spawns the play-mode child process;
- undo is kept across saves;
- frames rendered at viewport size.

The review also added extras: a Window no-window path, DuplicateEntity keeping outside references, autosave and `savedRevision` in the MVP, and `--create` being CPU-only.

**Three standalone fix PRs come before the MVP (E0):**
1. normalise `FrameData` to RGBA top-row-first and resize the renderer on `SetViewportSize`;
2. the host initialises `ResourceUUID`/`ResourceLoader` from `--project`, accepts `--port 0`, and prints a ready line;
3. stop per-call logging for polled commands.

Also fold into a small engine PR: update `docs/audio.html` ("no client plays audio" is now false, after frontend PR #1) and close #8.

**On approval:** post the proposal and a Decisions comment on #6 (the same format as #3), create one sub-issue per PR, and start E0.
**Frontend repo:** see [[electron-editor-frontend]]. Merge rules: [[merge-authorization]].
