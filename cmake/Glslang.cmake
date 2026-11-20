# glslang for the runtime shader compiler (user shaders, stage 7; orchestrator/proposals/user-shaders.md section 7).
#
# Included by engine/CMakeLists.txt when N2ENGINE_RUNTIME_SHADER_COMPILER is ON. A PINNED release source archive, SHA-256 checked
# (cmake/N2Download.cmake), built as part of this tree: it defines the static targets glslang, SPIRV and
# glslang-default-resource-limits. Only the compiler libraries are built (no command-line tools, no SPIRV-Tools optimizer, no HLSL,
# no remapper, no tests), so nothing else is fetched. BSD-3-Clause with the notices of THIRD_PARTY_NOTICES.md.
#
# The version is the one that made the committed fixtures (tests/renderer/fixtures/user-shaders: `Glslang Version: 11:15.3.0` in
# their manifests), so the cross-check test can compare SPIR-V byte for byte. To bump it: change the three values, take the hash
# from the file you pinned with sha256sum, and re-run scripts/cook-user-shader-fixtures.sh with the same glslang.
include(${CMAKE_CURRENT_LIST_DIR}/N2Download.cmake)

set(N2_GLSLANG_VERSION "15.3.0")
set(N2_GLSLANG_SHA256 "c6c21fe1873c37e639a6a9ac72d857ab63a5be6893a589f34e09a6c757174201")
set(N2_GLSLANG_URL "https://github.com/KhronosGroup/glslang/archive/refs/tags/${N2_GLSLANG_VERSION}.tar.gz")
set(N2_GLSLANG_DIR "${CMAKE_BINARY_DIR}/glslang-src")
set(N2_GLSLANG_STAMP "${N2_GLSLANG_DIR}/.archive-sha256")
set(N2_GLSLANG_ROOT "${N2_GLSLANG_DIR}/glslang-${N2_GLSLANG_VERSION}")

set(N2_GLSLANG_STAMP_OK OFF)
if(EXISTS "${N2_GLSLANG_ROOT}/CMakeLists.txt" AND EXISTS "${N2_GLSLANG_STAMP}")
    file(READ "${N2_GLSLANG_STAMP}" N2_GLSLANG_STAMP_TEXT)
    string(STRIP "${N2_GLSLANG_STAMP_TEXT}" N2_GLSLANG_STAMP_TEXT)
    if(N2_GLSLANG_STAMP_TEXT STREQUAL "${N2_GLSLANG_SHA256}")
        set(N2_GLSLANG_STAMP_OK ON)
    endif()
endif()

if(NOT N2_GLSLANG_STAMP_OK)
    message(STATUS "Downloading glslang ${N2_GLSLANG_VERSION} (SHA-256 checked)...")
    file(REMOVE_RECURSE "${N2_GLSLANG_DIR}")
    set(N2_GLSLANG_ARCHIVE "${CMAKE_BINARY_DIR}/glslang-${N2_GLSLANG_VERSION}.tar.gz")
    n2_download(URL "${N2_GLSLANG_URL}" DEST "${N2_GLSLANG_ARCHIVE}" SHA256 "${N2_GLSLANG_SHA256}" RESULT N2_GLSLANG_DOWNLOAD_MESSAGE)
    if(N2_GLSLANG_DOWNLOAD_MESSAGE)
        message(FATAL_ERROR "Could not fetch glslang ${N2_GLSLANG_VERSION} from ${N2_GLSLANG_URL}, or its SHA-256 is not the pinned "
                            "${N2_GLSLANG_SHA256}: ${N2_GLSLANG_DOWNLOAD_MESSAGE}")
    endif()
    file(MAKE_DIRECTORY "${N2_GLSLANG_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${N2_GLSLANG_ARCHIVE}"
        WORKING_DIRECTORY "${N2_GLSLANG_DIR}"
        RESULT_VARIABLE N2_GLSLANG_EXTRACT_RESULT
        ERROR_VARIABLE N2_GLSLANG_EXTRACT_ERROR)
    file(REMOVE "${N2_GLSLANG_ARCHIVE}")
    if(NOT N2_GLSLANG_EXTRACT_RESULT EQUAL 0 OR NOT EXISTS "${N2_GLSLANG_ROOT}/CMakeLists.txt")
        file(REMOVE_RECURSE "${N2_GLSLANG_DIR}")
        message(FATAL_ERROR "Could not extract glslang from its archive: ${N2_GLSLANG_EXTRACT_ERROR}")
    endif()
    file(WRITE "${N2_GLSLANG_STAMP}" "${N2_GLSLANG_SHA256}\n")
endif()

# glslang's own options, set before its CMake runs and left alone afterwards
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_WERROR OFF CACHE BOOL "" FORCE)
# glslang is a static library here whatever the rest of the tree builds; its position independence follows the tree
set(_n2_saved_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${N2_GLSLANG_ROOT}" "${CMAKE_BINARY_DIR}/glslang-build" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_n2_saved_shared}")
