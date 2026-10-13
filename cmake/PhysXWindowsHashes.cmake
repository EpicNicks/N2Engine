# Pinned PhysX Windows archive (the slim zip published on this repository's releases, derived from the original
# physx-dependencies bin.zip; the original build recipe is unknown). The root CMakeLists.txt downloads it and refuses it
# unless its SHA256 matches; a mismatch or failed download turns PhysX OFF with a warning.
# To move to a new build: publish it (asset physx-windows-x86_64-<version>-slim.zip on a release tagged
# physx-dependencies-windows-<version>), then change the version and hash here. The CI cache key hashes this file.
set(PHYSX_WINDOWS_VERSION "5.6.1")
set(PHYSX_WINDOWS_RELEASE_TAG "physx-dependencies-windows-5.6.1")
set(PHYSX_WINDOWS_SHA256 "27258b5ff6eae3e8e99243cca67bee6330d1bf48b41a51c4083466b8e2c91fa1")
