# The PhysX source the web (Emscripten) build compiles, pinned by commit. Included by cmake/PhysXWeb.cmake, and
# its values go to scripts/web-build-physx.sh. The native builds do not use this file: they download prebuilt
# archives (cmake/PhysXLinuxHashes.cmake, cmake/PhysXWindowsHashes.cmake).
#
# The tag is only the human name. What is pinned is the commit: scripts/web-build-physx.sh fetches that hash from
# https://github.com/NVIDIA-Omniverse/PhysX and stops unless the checkout's HEAD is exactly it (a git commit hash covers
# the whole tree, so a moved or re-created tag cannot change what is built). It is the same commit the Linux archives
# are built from (.github/workflows/physx-linux.yml), and its headers are the ones committed in external/PhysX/physx/include.
# To move to another PhysX: change the commit and tag here, check that the script's patches still apply (it fails if one
# does not), and bump PHYSX_WEB_BUILD_REV.
set(PHYSX_WEB_TAG "107.3-omni-and-physx-5.6.1")
set(PHYSX_WEB_COMMIT "09ff24f3279b735e672ff27b155cbf49f6296f4d")
set(PHYSX_WEB_VERSION "5.6.1")

# Bumped by hand when scripts/web-build-physx.sh changes how the libraries are built (flags, patches, library list). It
# is part of the stamp the built libraries carry and of the CI cache key, so a changed recipe never reuses old libraries.
set(PHYSX_WEB_BUILD_REV 2)
