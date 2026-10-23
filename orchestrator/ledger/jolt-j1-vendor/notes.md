Accepted by the user 2026-10-13:
- Added two web-only probe programs (cmake/jolt/probe) to measure the wasm delta without linking Jolt into the player
- Added scripts/check-jolt-vendor.sh, run in the required Clang leg, to check the vendored tree against its hash list
- Smoke-test golden hash 0x81a7425e66b56bb4 was taken from the first CI run (identical on all 7 legs)
- Web build of Jolt keeps exceptions and RTTI enabled (no -fno-exceptions on web)
- Docs-only fix round: CI minutes stated as measured step durations (the Jolt-only steps); the compile time Jolt adds to existing Build steps was not measurable without a no-Jolt run
