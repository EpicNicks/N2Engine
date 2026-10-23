Follow-ups to FE11 (#50, inspector multi-edit), two LOW review findings.

1. Remove-for-all no longer acts on the component list captured before the confirm dialog: if the selection changed while the dialog was open, nothing is sent and the call resolves false.
2. A refusal mid-batch rolls back: the edit group is ended (the host refuses Undo while one is open), then the page's Undo (EditController.undo, passed in as the `undo` option) takes back the applied edits, so settle, group close, history refresh and panel sync all happen as for the Undo menu. The panel says the edit was rolled back. Rollback is used only when the host opened a group, at least one change was applied, and no other edit is queued behind it (Undo would take back the wrong step). Otherwise (no group, nothing applied, no undo wired, another edit queued, or the undo failed) the previous message stays ("N applied, Undo takes back what was applied"), with the undo failure appended when it failed. The edit is counted finished before the undo runs, because Undo settles running edits first and would otherwise wait for itself.

Single-object edit paths are unchanged. Tests cover abort on changed selection, rollback, no rollback without a group, when nothing applied or when another edit is queued, and a failed rollback.

Local: npm run typecheck, npm run build, npm test pass.

Not verified: rollback against a real host (tests fake the page's undo); if Undo is a silent no-op (editing disabled meanwhile) the message would still say rolled back.
