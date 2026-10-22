Accepted by the user 2026-10-13:
- FE9b chose repair-on-Apply, only when a pair was edited; a rename alone still leaves an invalid matrix untouched (existing test requires it)
- 1.0 vs 1 fixed in settingsEdited (only invalid drafts count) rather than dropping the draft, to keep text like 1. while typing
