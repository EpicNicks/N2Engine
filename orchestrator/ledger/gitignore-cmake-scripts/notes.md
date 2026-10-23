# Notes (gitignore-cmake-scripts, 2026-10-13)
Approved deviations (accepted by the user):
- No code change: review was clean (one low, accepted: no cmake/CMakeLists.txt exists); CI failure was only the ci-wait script printing nothing
Accepted low: !cmake/*.cmake would also un-ignore a cmake_install.cmake generated directly in cmake/ (CMake writes it into the build folder).
Unverified: 3 windows jobs and linux x86_64 Clang still pending at last check; 4 other jobs passed
