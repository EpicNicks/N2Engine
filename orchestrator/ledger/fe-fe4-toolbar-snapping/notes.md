Accepted by the user 2026-10-13:
- Worktree already held uncommitted prior work; I finished and fixed it (3 failing draft tests, a bad settleForTest call) rather than rewriting.
- Snap settings are project-scope (roadmap says stored per project); tool/space/pivot are user-scope.
- Space button disabled for Scale and Select tools (scale handles are always local); Pivot disabled for Select.
- Absolute grid snap applies only to world-axis handles; local-axis handles stay relative.
- Snap toggles default off for all tools (Ctrl turns it on, as before).
- Review fix: local frame of a mirrored primary is made right-handed by flipping z (handles lie on the same lines).
- Review fix: after a drag the gizmo origin is held until reloadBounds resolves (reloadBounds now returns a promise); pointerDown uses it too.
- Review fix 2: loadTarget clears the held drag end when the selection key changes (or clears), not after a same-selection drag reload; test added.
