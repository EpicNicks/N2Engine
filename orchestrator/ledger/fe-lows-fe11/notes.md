Accepted by the user 2026-10-13:
- Rollback goes through the page's EditController.undo via a new undo option (wired in index.tsx) instead of raw engine.undo; runs after the edit is counted finished to avoid a settle deadlock
- Rollback is skipped when another edit is queued behind the refused one (message keeps the Undo-takes-back text)
- Remove-for-all with a changed selection aborts (returns false) rather than re-reading the list
- Dropped the Partial undo pick on the inspector Engine type (no longer needed)
