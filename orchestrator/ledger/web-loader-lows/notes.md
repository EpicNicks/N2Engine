Accepted by the user 2026-10-13:
- Tests added to scripts/web-loader-test.mjs (the CI node test), not tests/web
- Cancellation done by controller.error() on the wrapping TransformStream plus an abandoned flag; the old unlocked-only cancel removed
- No shell/docs change: shell has no per-kind message place, docs already accurate
