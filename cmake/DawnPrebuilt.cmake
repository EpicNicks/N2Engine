# Dawn for the WebGPU backend's native CI harness (#155, stage G2): a PINNED PREBUILT release, not a source build.
#
# Included by renderer/CMakeLists.txt when N2ENGINE_RENDERER_WEBGPU is ON. It defines the imported static library target
# n2_dawn (libwebgpu_dawn.a, Dawn's monolithic library with its Vulkan backend, plus the webgpu.h headers) and fails the
# configure, with the reason, when it cannot.
#
# Why prebuilt: Dawn's own release archive for Linux (https://github.com/google/dawn/releases) holds the library and the C
# headers built from ONE commit, and it is the very archive scripts/install-wgsl-validators.sh takes Tint from, so the shader
# validator and the renderer's Dawn are the same revision by construction. Building Dawn from source needs depot_tools-style
# dependency fetching or a dozen pinned submodules and 15-30 minutes of CI per cold cache; this is a 36 MB download checked
# against the SHA-256 below (no build, so no ccache or build cache to tune). The archive is a GCC 14 build with the libstdc++
# C++11 ABI, position-independent, which the GCC 14 and Clang + libstdc++ 14 legs of the CI link against.
#
# Linux x86_64 only: it is a CI harness (design decision W8), never a shipped renderer, and Dawn publishes the one archive.
#
# To bump Dawn: pick a release tag, set the four values below (the commit is in the archive's name; take the hash from the
# file you pinned with sha256sum, not from a web page), and set the same tag, commit and hash in
# scripts/install-wgsl-validators.sh so Tint moves with it. The emdawnwebgpu port (stage G4) pins its own Dawn revision through
# the Emscripten version: see docs/webgpu.html, "Bumping Dawn".
set(N2_DAWN_TAG "v20261016.190333")
set(N2_DAWN_COMMIT "67d4d6e64b0755d638f76549794692badfdbc8ab")
set(N2_DAWN_SHA256 "20ff88448def9645ceeff30911e07295aeb433be23e26180c96019b4300bff5a")
set(N2_DAWN_STEM "Dawn-${N2_DAWN_COMMIT}-ubuntu-latest-Release")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "N2ENGINE_RENDERER_WEBGPU=ON is the CI test harness of the WebGPU backend and is supported on Linux "
                        "x86_64 only (Dawn's prebuilt release); this is ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}. "
                        "Leave it OFF (the default).")
endif()

set(N2_DAWN_DIR "${CMAKE_BINARY_DIR}/dawn-prebuilt")
set(N2_DAWN_ARCHIVE "${CMAKE_BINARY_DIR}/${N2_DAWN_STEM}.tar.gz")
set(N2_DAWN_URL "https://github.com/google/dawn/releases/download/${N2_DAWN_TAG}/${N2_DAWN_STEM}.tar.gz")
set(N2_DAWN_LIBRARY "${N2_DAWN_DIR}/${N2_DAWN_STEM}/lib64/libwebgpu_dawn.a")
set(N2_DAWN_INCLUDE_DIR "${N2_DAWN_DIR}/${N2_DAWN_STEM}/include")
# Written after a verified extraction: a tree whose stamp names another hash (the pin changed) is fetched again
set(N2_DAWN_STAMP "${N2_DAWN_DIR}/.archive-sha256")

set(N2_DAWN_STAMP_OK OFF)
if(EXISTS "${N2_DAWN_LIBRARY}" AND EXISTS "${N2_DAWN_INCLUDE_DIR}/webgpu/webgpu.h" AND EXISTS "${N2_DAWN_STAMP}")
    file(READ "${N2_DAWN_STAMP}" N2_DAWN_STAMP_TEXT)
    string(STRIP "${N2_DAWN_STAMP_TEXT}" N2_DAWN_STAMP_TEXT)
    if(N2_DAWN_STAMP_TEXT STREQUAL "${N2_DAWN_SHA256}")
        set(N2_DAWN_STAMP_OK ON)
    endif()
endif()

if(N2_DAWN_STAMP_OK)
    message(STATUS "Dawn ${N2_DAWN_TAG} (${N2_DAWN_COMMIT}): already extracted in ${N2_DAWN_DIR}")
else()
    message(STATUS "Downloading Dawn ${N2_DAWN_TAG} (${N2_DAWN_STEM}.tar.gz, SHA-256 checked)...")
    file(REMOVE_RECURSE "${N2_DAWN_DIR}")
    file(DOWNLOAD "${N2_DAWN_URL}" "${N2_DAWN_ARCHIVE}"
         EXPECTED_HASH SHA256=${N2_DAWN_SHA256}
         TLS_VERIFY ON
         STATUS N2_DAWN_DOWNLOAD_STATUS)
    list(GET N2_DAWN_DOWNLOAD_STATUS 0 N2_DAWN_DOWNLOAD_CODE)
    list(GET N2_DAWN_DOWNLOAD_STATUS 1 N2_DAWN_DOWNLOAD_MESSAGE)
    if(NOT N2_DAWN_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE "${N2_DAWN_ARCHIVE}")
        message(FATAL_ERROR "Could not fetch Dawn ${N2_DAWN_TAG} from ${N2_DAWN_URL}, or its SHA-256 is not the pinned "
                            "${N2_DAWN_SHA256}: ${N2_DAWN_DOWNLOAD_MESSAGE}")
    endif()

    # The whole archive is extracted (cmake -E tar), then what the renderer does not link is deleted: Tint's libraries and
    # the Tint and fuzzer tools are most of its size, and none of it is used here (scripts/install-wgsl-validators.sh takes
    # the tint tool from the same archive for the shader job)
    file(MAKE_DIRECTORY "${N2_DAWN_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${N2_DAWN_ARCHIVE}"
        WORKING_DIRECTORY "${N2_DAWN_DIR}"
        RESULT_VARIABLE N2_DAWN_EXTRACT_RESULT
        ERROR_VARIABLE N2_DAWN_EXTRACT_ERROR)
    file(REMOVE "${N2_DAWN_ARCHIVE}")
    if(NOT N2_DAWN_EXTRACT_RESULT EQUAL 0 OR NOT EXISTS "${N2_DAWN_LIBRARY}" OR NOT EXISTS "${N2_DAWN_INCLUDE_DIR}/webgpu/webgpu.h")
        file(REMOVE_RECURSE "${N2_DAWN_DIR}")
        message(FATAL_ERROR "Could not extract libwebgpu_dawn.a and the headers from the Dawn archive: ${N2_DAWN_EXTRACT_ERROR}")
    endif()
    file(GLOB N2_DAWN_UNUSED_LIBRARIES "${N2_DAWN_DIR}/${N2_DAWN_STEM}/lib64/libtint_*.a")
    file(REMOVE ${N2_DAWN_UNUSED_LIBRARIES})
    file(REMOVE_RECURSE "${N2_DAWN_DIR}/${N2_DAWN_STEM}/bin")
    file(WRITE "${N2_DAWN_STAMP}" "${N2_DAWN_SHA256}\n")
    message(STATUS "Dawn ${N2_DAWN_TAG} extracted to ${N2_DAWN_DIR}")
endif()

# Dawn loads libvulkan and the display libraries with dlopen, so the library itself needs only the C library's own
find_package(Threads REQUIRED)
add_library(n2_dawn STATIC IMPORTED GLOBAL)
set_target_properties(n2_dawn PROPERTIES IMPORTED_LOCATION "${N2_DAWN_LIBRARY}")
# SYSTEM: Dawn's headers are not ours to warn about. <webgpu/webgpu.h> is the C API (it includes dawn/webgpu.h)
target_include_directories(n2_dawn SYSTEM INTERFACE "${N2_DAWN_INCLUDE_DIR}")
target_link_libraries(n2_dawn INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
