Accepted by the user 2026-10-13:
- Row/column select-all helpers act only on the shown layers (hidden unnamed layers are left unchanged); tri-state, clears when all shown pairs are on
- Full 32x32 symmetric grid (both triangles) rather than Unity's triangular layout; one edit key per pair
- An invalid project matrix (not 32 masks) is shown as all-colliding with a note; Apply writes a valid full matrix
- Added ProjectSettingsModel.setEdits (batch edit) to project-settings-state.ts
- Edited one sentence of the FE9 README section (the now-false read-only note) besides adding the new section; new section placed after Project settings, not at file end
- CSS added to src/index.html for the grid
