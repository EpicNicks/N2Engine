# PhysX for the web (Emscripten) build: static CPU-only libraries compiled from the pinned PhysX source by
# scripts/web-build-physx.sh. Included from the root CMakeLists.txt only when N2ENGINE_PLATFORM_WEB and N2ENGINE_USE_PHYSX
# are ON. Sets what the Linux and Windows branches set: N2ENGINE_PHYSX_AVAILABLE, PHYSX_INCLUDE_DIR, PHYSX_LIBS and
# PHYSX_DLLS (empty); on any failure it sets N2ENGINE_USE_PHYSX OFF with a warning, as they do. docs/web-build.html.
#
# The libraries are built once per checkout (and once per CI cache key): they go to PHYSX_WEB_DIR, which is under the
# git-ignored external/PhysX/physx/bin, and every web build directory (dev, size, speed) reuses them. They are reused
# only when the stamp the script wrote (.pin) names the pinned commit, the build revision and this compiler's version.

include(${CMAKE_SOURCE_DIR}/cmake/PhysXWebPin.cmake)

set(PHYSX_WEB_DIR "${CMAKE_SOURCE_DIR}/external/PhysX/physx/bin/emscripten.wasm32" CACHE PATH
    "Where the web PhysX libraries are built to and reused from (lib/, include/, .pin)")

# The order is the link order: the archives refer to each other only in this direction (the web linker does not rescan)
set(PHYSX_WEB_LIB_NAMES
    PhysXExtensions_static_32
    PhysX_static_32
    PhysXPvdSDK_static_32
    PhysXCommon_static_32
    PhysXFoundation_static_32
)
set(PHYSX_WEB_LIB_FILES "")
foreach(lib IN LISTS PHYSX_WEB_LIB_NAMES)
    list(APPEND PHYSX_WEB_LIB_FILES "${PHYSX_WEB_DIR}/lib/lib${lib}.a")
endforeach()

set(PHYSX_WEB_STAMP_EXPECTED "commit=${PHYSX_WEB_COMMIT}\nbuild-rev=${PHYSX_WEB_BUILD_REV}\ncompiler=${CMAKE_CXX_COMPILER_VERSION}")

function(n2_physx_web_present out_var)
    set(present ON)
    foreach(file IN LISTS PHYSX_WEB_LIB_FILES)
        if(NOT EXISTS "${file}")
            set(present OFF)
        endif()
    endforeach()
    if(present AND NOT EXISTS "${PHYSX_WEB_DIR}/include/PxPhysicsAPI.h")
        set(present OFF)
    endif()
    if(present AND EXISTS "${PHYSX_WEB_DIR}/.pin")
        file(READ "${PHYSX_WEB_DIR}/.pin" stamp)
        string(STRIP "${stamp}" stamp)
        if(NOT stamp STREQUAL PHYSX_WEB_STAMP_EXPECTED)
            set(present OFF)
        endif()
    else()
        set(present OFF)
    endif()
    set(${out_var} ${present} PARENT_SCOPE)
endfunction()

n2_physx_web_present(PHYSX_WEB_PRESENT)
if(NOT PHYSX_WEB_PRESENT)
    find_program(PHYSX_WEB_BASH bash)
    get_filename_component(PHYSX_WEB_EMDIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(NOT PHYSX_WEB_BASH)
        message(WARNING "Building PhysX for the web needs bash (scripts/web-build-physx.sh). Disabling PhysX.")
        set(N2ENGINE_USE_PHYSX OFF)
    else()
        message(STATUS "Building PhysX ${PHYSX_WEB_VERSION} (${PHYSX_WEB_COMMIT}) for WebAssembly into ${PHYSX_WEB_DIR} (once; a few minutes)...")
        execute_process(
            COMMAND "${PHYSX_WEB_BASH}" "${CMAKE_SOURCE_DIR}/scripts/web-build-physx.sh"
                    "${PHYSX_WEB_COMMIT}" "${PHYSX_WEB_DIR}" "${PHYSX_WEB_EMDIR}" "${PHYSX_WEB_BUILD_REV}" "${CMAKE_CXX_COMPILER_VERSION}"
            RESULT_VARIABLE PHYSX_WEB_RESULT
            TIMEOUT 5400
        )
        n2_physx_web_present(PHYSX_WEB_PRESENT)
        if(NOT PHYSX_WEB_RESULT EQUAL 0 OR NOT PHYSX_WEB_PRESENT)
            message(WARNING "Building PhysX for the web failed (exit status: ${PHYSX_WEB_RESULT}). Disabling PhysX.")
            set(N2ENGINE_USE_PHYSX OFF)
        endif()
    endif()
endif()

if(N2ENGINE_USE_PHYSX AND PHYSX_WEB_PRESENT)
    set(PHYSX_INCLUDE_DIR "${PHYSX_WEB_DIR}/include")
    set(PHYSX_LIBS ${PHYSX_WEB_LIB_FILES})
    set(PHYSX_DLLS "")
    set(N2ENGINE_PHYSX_AVAILABLE ON)
endif()
