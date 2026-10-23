# Notes (windows-physx-slim-build, 2026-10-13)
Approved deviations (accepted by the user):
- MANIFEST.txt and any root LICENSE* now live in bin/win.x86_64.vc143.mt/ so they stay git-ignored on extraction
- EXPECTED_SOURCE_SHA256 pinned to 77eedc4b...6d527 (1405731918 bytes, from PR run 38426123539)
- Release creation uses --prerelease --latest=false (dependency release, not an engine release).
Slim zip artifact about 535 MB vs bin.zip 1.34 GB (about 62% smaller). Keep-list parsed from the real root CMakeLists; GPU dll + import lib kept in debug and release; *Gpu_static libs, snippets, other exes, .map/.exp, checked/profile dropped. Provenance: derived from bin.zip sha256 77eedc4b...6d527 (1405731918 bytes); the original build recipe is unknown. Release `physx-dependencies-windows-5.6.1` to be created as a PRERELEASE by the orchestrator and published via workflow_dispatch publish=true.
