# Notes (physx-linux-build, 2026-10-13)
Approved deviations (accepted by the user):
- PhysX output dir is bin/linux.x86_64/release and bin/linux.aarch64/release, not linux.clang
- Library set is eight archives (SceneQuery/LowLevel/SimulationController are folded into libPhysX_static_64.a; Vehicle2 instead of Vehicle)
- Clang 20 built PhysX with no source patch beyond removing ' -Werror' via sed plus -w in CMAKE_CXX_FLAGS; Clang 18 fallback not needed
- Review fix: archives reproducible (sorted, mtime 0, owner 0, gzip -n) and release upload no longer uses --clobber, so a repeat publish fails rather than overwriting pinned assets
- PVD runtime (OmniPVD project) built off (PX_BUILDPVDRUNTIME=False); PxPvdSDK static lib is still included
- Smoke test includes headers from committed external/PhysX with -isystem to silence PhysX header warnings
- Publish job creates the release (target = run SHA) if it does not exist
- Review fix: tag is 107.3-omni-and-physx-5.6.1 (commit 09ff24f3); header diff on run 38406397125 shows only 4 differing headers plus generated PxConfig.h; notes.json physxCommit/headerDiff refreshed (ledger file, outside repo)
See notes.json for the library list, link order, archive layout, header diff (safe: CRLF + trailing // deprecated comments only) and build command. Publish run (workflow_dispatch publish=true) is done by the orchestrator after merge. Unverified: Publish job (needs a tag push or publish=true run after merge); Engine consuming the archives (PR 2)
