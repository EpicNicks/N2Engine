# Pinned PhysX Linux static archives (built by .github/workflows/physx-linux.yml from PhysX tag
# 107.3-omni-and-physx-5.6.1, published on this repository's releases). The root CMakeLists.txt downloads the archive
# for the host architecture and refuses it unless its SHA256 matches; a mismatch turns PhysX OFF with a warning.
# To move to a new build: publish it, then change the tag and both hashes here (and PHYSX_LINUX_VERSION, which keys
# the CI cache).
set(PHYSX_LINUX_VERSION "5.6.1")
set(PHYSX_LINUX_RELEASE_TAG "physx-dependencies-linux-5.6.1")
set(PHYSX_LINUX_SHA256_x86_64 "e8bd1f8f8d9f0b1d04d5f21ccdc509535ca5e56d23c0440b32f9aea5696208f5")
set(PHYSX_LINUX_SHA256_aarch64 "22c1fffdb3a81afd38820c68745ef22abfbe687786aef9c032bb947ffacabb90")
