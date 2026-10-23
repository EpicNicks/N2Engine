Accepted by the user 2026-10-13:
- Layers shows names plus a read-only note for the collision matrix (in GetProjectInfo but edited only in FE9b)
- No Edit menu entry: panel opens via Window menu only (Edit menu is outside this lane; reviewer low left as declared)
- Stale-draft fix: Raw/Input record a base text on first edit (snapshotKeys in SectionLogic) and Apply sends the draft's change from that base
- Tab-switch fix: dropped unmountWhenHidden for projectSettings so the model survives tab switches (closing the panel still drops drafts)
- Raw confirm message now shows the merge patch (confirm.message may be a function of the built section)
- Clearing all three gravity fields now sends physics.gravity null
- Restore on Raw/Input now records the post-apply text as the draft base, so it reverts only that Apply's changes
