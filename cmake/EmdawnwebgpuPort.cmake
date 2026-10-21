# The WebGPU bindings of the web build (#155, stage G4): Emscripten's emdawnwebgpu port, PINNED to the same Dawn release as the
# native CI harness (cmake/DawnPrebuilt.cmake), so the browser build and the harness compile the backend against the same
# webgpu.h.
#
# Included by renderer/CMakeLists.txt when N2ENGINE_RENDERER_WEBGPU is ON in the web build. It defines the INTERFACE target
# n2_emdawnwebgpu, whose compile and link option is --use-port=<the pinned remote port file>: the compile option gives the
# renderer <webgpu/webgpu.h>, the link option adds the port's implementation (libemdawnwebgpu, compiled by emcc into its
# cache, and library_webgpu.js, which calls the browser's navigator.gpu) to every executable that links the renderer.
#
# Why this port and not Emscripten's own: emcc 4.0.10 (the pinned EMSCRIPTEN_VERSION of the web CI job) ships a built-in
# `--use-port=emdawnwebgpu` that pins Dawn v20250531.224602, a webgpu.h more than a year older than the Dawn the harness is
# built and tested against (v20261016.190333). Dawn publishes, with every release, a "remote port" file
# (emdawnwebgpu-<tag>.remoteport.py, for Emscripten 4.0.10 and later) that makes emcc fetch that release's emdawnwebgpu
# package (emdawnwebgpu_pkg-<tag>.zip, pinned by a SHA-512 inside the remote port file). Using the harness's own tag keeps one
# Dawn revision for both builds (the design's "header parity"). The remote port file is downloaded here, checked against the
# SHA-256 below, and kept in the build folder; emcc downloads the package itself (into its cache) the first time it compiles
# with the flag. The old -sUSE_WEBGPU bindings are not used.
#
# To bump: take the tag of cmake/DawnPrebuilt.cmake (both move together: the "Dawn, Tint and emdawnwebgpu pins agree" step of
# the webgpu CI job fails when the tags differ), set N2_EMDAWNWEBGPU_TAG and the SHA-256 of the release's
# emdawnwebgpu-<tag>.remoteport.py (the release page lists a digest per asset), and run the web CI job.
set(N2_EMDAWNWEBGPU_TAG "v20261016.190333")
set(N2_EMDAWNWEBGPU_SHA256 "725a0174df01e80bab07134ea28c929126a6f7bf41c858ad1702b025fb5e10e4")

set(N2_EMDAWNWEBGPU_DIR "${CMAKE_BINARY_DIR}/emdawnwebgpu")
set(N2_EMDAWNWEBGPU_PORT_FILE "${N2_EMDAWNWEBGPU_DIR}/emdawnwebgpu-${N2_EMDAWNWEBGPU_TAG}.remoteport.py")
set(N2_EMDAWNWEBGPU_URL
    "https://github.com/google/dawn/releases/download/${N2_EMDAWNWEBGPU_TAG}/emdawnwebgpu-${N2_EMDAWNWEBGPU_TAG}.remoteport.py")

set(N2_EMDAWNWEBGPU_HAVE OFF)
if(EXISTS "${N2_EMDAWNWEBGPU_PORT_FILE}")
    file(SHA256 "${N2_EMDAWNWEBGPU_PORT_FILE}" N2_EMDAWNWEBGPU_EXISTING_SHA256)
    if(N2_EMDAWNWEBGPU_EXISTING_SHA256 STREQUAL "${N2_EMDAWNWEBGPU_SHA256}")
        set(N2_EMDAWNWEBGPU_HAVE ON)
    endif()
endif()
if(NOT N2_EMDAWNWEBGPU_HAVE)
    message(STATUS "Downloading the emdawnwebgpu remote port of Dawn ${N2_EMDAWNWEBGPU_TAG} (SHA-256 checked)...")
    file(MAKE_DIRECTORY "${N2_EMDAWNWEBGPU_DIR}")
    file(DOWNLOAD "${N2_EMDAWNWEBGPU_URL}" "${N2_EMDAWNWEBGPU_PORT_FILE}"
         EXPECTED_HASH SHA256=${N2_EMDAWNWEBGPU_SHA256}
         TLS_VERIFY ON
         STATUS N2_EMDAWNWEBGPU_STATUS)
    list(GET N2_EMDAWNWEBGPU_STATUS 0 N2_EMDAWNWEBGPU_CODE)
    list(GET N2_EMDAWNWEBGPU_STATUS 1 N2_EMDAWNWEBGPU_MESSAGE)
    if(NOT N2_EMDAWNWEBGPU_CODE EQUAL 0)
        file(REMOVE "${N2_EMDAWNWEBGPU_PORT_FILE}")
        message(FATAL_ERROR "Could not fetch the emdawnwebgpu remote port from ${N2_EMDAWNWEBGPU_URL}, or its SHA-256 is not "
                            "the pinned ${N2_EMDAWNWEBGPU_SHA256}: ${N2_EMDAWNWEBGPU_MESSAGE}")
    endif()
endif()
message(STATUS "Web: WebGPU bindings from emdawnwebgpu (Dawn ${N2_EMDAWNWEBGPU_TAG}): ${N2_EMDAWNWEBGPU_PORT_FILE}")

add_library(n2_emdawnwebgpu INTERFACE)
target_compile_options(n2_emdawnwebgpu INTERFACE "--use-port=${N2_EMDAWNWEBGPU_PORT_FILE}")
target_link_options(n2_emdawnwebgpu INTERFACE "--use-port=${N2_EMDAWNWEBGPU_PORT_FILE}")
