# Notes (physx-linux-enable, 2026-10-13)
Approved deviations (accepted by the user):
- cmake/PhysXLinuxHashes.cmake is gitignored (*.cmake); force-added with git add -f, .gitignore untouched (not an allowed path)
- Health state is named Running, not Ready; smoke adds --expect-physics on|off|any (default any) and a new --scenario attach used by the docker job against the running container
- Cache key uses hashFiles('cmake/PhysXLinuxHashes.cmake') (covers pinned version+hashes) plus runner.os/arch; CMake also keeps a hash stamp in bin/ and re-downloads on mismatch
- PVD gated by defining N2ENGINE_PHYSX_HAS_PVD only under _WIN32 in PhysXBackend.hpp
- Linux libs linked by full path from one release dir, so no per-config $<IF:$<CONFIG:Debug>> mapping; Debug no longer defines NDEBUG (header check is Windows-only)
- Download has no EXPECTED_HASH (it hard-errors); SHA256 checked with file(SHA256) after download, mismatch or failure warns and turns PhysX OFF
- Archive LICENSE.md moved into bin/linux.<arch>/release/ after extract so no untracked file; README GCC line now says not tested with PhysX
Follow-up: add `!cmake/*.cmake` to .gitignore (tiny PR). Debug on Linux uses release PhysX libs (diagnostics only, ABI-compatible); a checked/debug Linux PhysX asset is NOT planned. Unverified: physics behaviour under real gameplay on Linux; arm64 PhysX NEON path under load; bad-hash/offline fallback to PhysX OFF reproduced only by reasoning, not run in CI; CI for the fix commit not yet observed
