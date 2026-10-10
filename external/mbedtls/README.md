# Mbed TLS

Mbed TLS 3.6.7 (the 3.6 long-term-support line), copied unmodified from the release tarball
https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 (tag `mbedtls-3.6.7`).
Dual licensed: Apache-2.0 OR GPL-2.0-or-later, at your choice; `LICENSE` here is the upstream file. This project uses it
under Apache-2.0.

Only what a build needs is kept: `library/*.c` and `library/*.h`, and `include/` (`mbedtls/` and `psa/`). The upstream
build files, tests, programs, docs, scripts and the optional 3rdparty drivers (Everest, p256-m) are left out. The default
`mbedtls_config.h` is used as shipped: there is no project configuration file.

It is optional. It is compiled only when the CMake option `N2ENGINE_EDITOR_TLS` is ON (off by default), into the static
library `n2_mbedtls` that `editor-server/CMakeLists.txt` defines from `library/*.c`. Only
`editor-server/src/EditorTls.cpp` uses it in the host; the tests that act as a TLS client include it too. See
`docs/logging-and-editor.html#editor-tls`.

To update: replace `library/` and `include/` with the new release's, keep this file's version and tag current, and change
the version in `THIRD_PARTY_NOTICES.md`.
