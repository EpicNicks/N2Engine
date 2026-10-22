Accepted by the user 2026-10-13:
- Blocker fixed by renaming helper Run to RunRange
- Medium: dropped the bit-exact claim for the collider-spec rebuild path (test now checks mass and shape count only; docs say so) rather than avoiding RebuildBody
- Low: thread-count test now prints the listener thread count instead of asserting >=2; docs say it is not a gate; the NoteContactThread diagnostic mutex is left in place
- Low: docs no longer tell callers to call RefreshCollisionMatrix after a restore
- Review low (testing.html row overstated): weakened the row and added the unchecked collider properties to the physics.html Not verified list; merged origin/master (clean)
