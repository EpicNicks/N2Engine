cmake_minimum_required(VERSION 3.25)
project(. VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(/MP)
endif()

option(N2ENGINE_BUILD_TESTS "Build unit tests" ON)
option(N2ENGINE_USE_PHYSX "Use PhysX physics backend" ON)
# OFF compiles the Vulkan renderer out: the build then needs no Vulkan headers or loader, and --renderer vulkan / the
# VULKAN render backend fail with a clear message. ON (the default) needs the Vulkan SDK (Windows) or libvulkan-dev (Linux).
option(N2ENGINE_RENDERER_VULKAN "Build the Vulkan renderer backend" ON)

# == PhysX setup ==
set(N2ENGINE_PHYSX_AVAILABLE OFF)

if(N2ENGINE_USE_PHYSX)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(WARNING
                "PhysX is not supported with MinGW/GCC on Windows. "
                "Disabling PhysX. Use MSVC or clang-cl for PhysX support."
        )
        set(N2ENGINE_USE_PHYSX OFF)
    elseif(WIN32)
        set(PHYSX_ROOT "${CMAKE_SOURCE_DIR}/external/PhysX")
        set(PHYSX_BIN_DIR "${PHYSX_ROOT}/physx/bin")
        # Slim archive pinned by SHA256 in cmake/PhysXWindowsHashes.cmake; any failure below turns PhysX OFF
        include(${CMAKE_SOURCE_DIR}/cmake/PhysXWindowsHashes.cmake)
        set(PHYSX_WINDOWS_ARCHIVE_NAME "physx-windows-x86_64-${PHYSX_WINDOWS_VERSION}-slim.zip")
        set(PHYSX_ZIP "${CMAKE_BINARY_DIR}/${PHYSX_WINDOWS_ARCHIVE_NAME}")
        set(PHYSX_DOWNLOAD_URL "https://github.com/EpicNicks/N2Engine/releases/download/${PHYSX_WINDOWS_RELEASE_TAG}/${PHYSX_WINDOWS_ARCHIVE_NAME}")
        set(PHYSX_WINDOWS_DIR "${PHYSX_BIN_DIR}/win.x86_64.vc143.mt")
        # Written after a verified extraction (inside bin/, so a CI cache of bin/ carries it). A tree without a stamp
        # (the old bin.zip, same file names) is reused as is; a stamp naming another hash means the pin changed.
        set(PHYSX_WINDOWS_STAMP "${PHYSX_WINDOWS_DIR}/.archive-sha256")
        set(PHYSX_LIB_DIR ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>)
        set(PHYSX_INCLUDE_DIR ${PHYSX_ROOT}/physx/include)
        set(PHYSX_LIBS
                PhysX_64
                PhysXCommon_64
                PhysXFoundation_64
                PhysXCooking_64
                PhysXExtensions_static_64
                PhysXPvdSDK_static_64
        )
        set(PHYSX_DLLS
                PhysX_64.dll
                PhysXCommon_64.dll
                PhysXFoundation_64.dll
                PhysXCooking_64.dll
        )

        set(PHYSX_WINDOWS_REUSE OFF)
        if(EXISTS "${PHYSX_WINDOWS_DIR}/debug/PhysX_64.dll")
            set(PHYSX_WINDOWS_REUSE ON)
            if(EXISTS "${PHYSX_WINDOWS_STAMP}")
                file(READ "${PHYSX_WINDOWS_STAMP}" PHYSX_WINDOWS_STAMP_TEXT)
                string(STRIP "${PHYSX_WINDOWS_STAMP_TEXT}" PHYSX_WINDOWS_STAMP_TEXT)
                if(NOT PHYSX_WINDOWS_STAMP_TEXT STREQUAL PHYSX_WINDOWS_SHA256)
                    set(PHYSX_WINDOWS_REUSE OFF)
                endif()
            endif()
        endif()

        if(PHYSX_WINDOWS_REUSE)
            set(N2ENGINE_PHYSX_AVAILABLE ON)
        else()
            message(STATUS "Downloading PhysX Windows binaries (${PHYSX_WINDOWS_ARCHIVE_NAME})...")
            # No EXPECTED_HASH: it makes a failed download or a mismatch a hard CMake error, and the plan is to fall
            # back to PhysX OFF. The hash is checked below, before anything is extracted.
            file(DOWNLOAD "${PHYSX_DOWNLOAD_URL}" "${PHYSX_ZIP}" SHOW_PROGRESS STATUS DOWNLOAD_STATUS TIMEOUT 1200)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)
            set(PHYSX_WINDOWS_ARCHIVE_SHA256 "")
            if(STATUS_CODE EQUAL 0)
                file(SHA256 "${PHYSX_ZIP}" PHYSX_WINDOWS_ARCHIVE_SHA256)
            endif()
            if(NOT STATUS_CODE EQUAL 0)
                file(REMOVE "${PHYSX_ZIP}")
                message(WARNING "Failed to download PhysX Windows binaries: ${STATUS_MESSAGE}. Disabling PhysX.")
                set(N2ENGINE_USE_PHYSX OFF)
            elseif(NOT PHYSX_WINDOWS_ARCHIVE_SHA256 STREQUAL PHYSX_WINDOWS_SHA256)
                file(REMOVE "${PHYSX_ZIP}")
                message(WARNING "The PhysX Windows archive's SHA256 (${PHYSX_WINDOWS_ARCHIVE_SHA256}) does not match the pinned one (${PHYSX_WINDOWS_SHA256}). Disabling PhysX.")
                set(N2ENGINE_USE_PHYSX OFF)
            else()
                file(REMOVE_RECURSE "${PHYSX_WINDOWS_DIR}")
                file(ARCHIVE_EXTRACT INPUT "${PHYSX_ZIP}" DESTINATION "${PHYSX_ROOT}/physx")
                file(REMOVE "${PHYSX_ZIP}")
                if(EXISTS "${PHYSX_WINDOWS_DIR}/debug/PhysX_64.dll" AND EXISTS "${PHYSX_WINDOWS_DIR}/release/PhysX_64.dll")
                    file(WRITE "${PHYSX_WINDOWS_STAMP}" "${PHYSX_WINDOWS_SHA256}
")
                    set(N2ENGINE_PHYSX_AVAILABLE ON)
                else()
                    message(WARNING "The PhysX Windows archive has no debug/ and release/ PhysX_64.dll. Disabling PhysX.")
                    set(N2ENGINE_USE_PHYSX OFF)
                endif()
            endif()
        endif()

        if(N2ENGINE_PHYSX_AVAILABLE AND EXISTS "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/debug/PhysXGpu_64.dll")
            list(APPEND PHYSX_LIBS PhysXGpu_64)
            list(APPEND PHYSX_DLLS PhysXGpu_64.dll)
            message(STATUS "PhysX GPU support enabled")
        endif()
    elseif(UNIX AND NOT APPLE)
        # Static, Release-only libraries (also used by Debug and RelWithDebInfo builds), built from the PhysX tag the
        # committed headers come from and pinned by SHA256 in cmake/PhysXLinuxHashes.cmake. Any failure below turns
        # PhysX OFF: an archive whose hash does not match is never used.
        include(${CMAKE_SOURCE_DIR}/cmake/PhysXLinuxHashes.cmake)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            set(PHYSX_LINUX_ARCH x86_64)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(PHYSX_LINUX_ARCH aarch64)
        else()
            set(PHYSX_LINUX_ARCH "")
            message(WARNING "No PhysX Linux binaries for ${CMAKE_SYSTEM_PROCESSOR}. Disabling PhysX.")
            set(N2ENGINE_USE_PHYSX OFF)
        endif()

        if(PHYSX_LINUX_ARCH)
            set(PHYSX_ROOT "${CMAKE_SOURCE_DIR}/external/PhysX")
            set(PHYSX_INCLUDE_DIR ${PHYSX_ROOT}/physx/include)
            set(PHYSX_LIB_DIR ${PHYSX_ROOT}/physx/bin/linux.${PHYSX_LINUX_ARCH}/release)
            set(PHYSX_LINUX_SHA256 "${PHYSX_LINUX_SHA256_${PHYSX_LINUX_ARCH}}")
            set(PHYSX_LINUX_ARCHIVE_NAME "physx-linux-${PHYSX_LINUX_ARCH}-${PHYSX_LINUX_VERSION}.tar.gz")
            set(PHYSX_LINUX_ARCHIVE "${CMAKE_BINARY_DIR}/${PHYSX_LINUX_ARCHIVE_NAME}")
            set(PHYSX_LINUX_URL "https://github.com/EpicNicks/N2Engine/releases/download/${PHYSX_LINUX_RELEASE_TAG}/${PHYSX_LINUX_ARCHIVE_NAME}")
            # Written after a verified extraction (inside bin/, so a CI cache of bin/ carries it): the libraries are
            # reused only when it names the pinned hash
            set(PHYSX_LINUX_STAMP "${PHYSX_LIB_DIR}/.archive-sha256")

            set(PHYSX_LINUX_LIB_NAMES
                    PhysX_static_64
                    PhysXCommon_static_64
                    PhysXFoundation_static_64
                    PhysXCooking_static_64
                    PhysXExtensions_static_64
                    PhysXPvdSDK_static_64
                    PhysXCharacterKinematic_static_64
                    PhysXVehicle2_static_64
            )
            set(PHYSX_LINUX_LIB_FILES "")
            foreach(lib IN LISTS PHYSX_LINUX_LIB_NAMES)
                list(APPEND PHYSX_LINUX_LIB_FILES "${PHYSX_LIB_DIR}/lib${lib}.a")
            endforeach()

            set(PHYSX_LINUX_PRESENT ON)
            foreach(file IN LISTS PHYSX_LINUX_LIB_FILES)
                if(NOT EXISTS "${file}")
                    set(PHYSX_LINUX_PRESENT OFF)
                endif()
            endforeach()
            set(PHYSX_LINUX_STAMP_OK OFF)
            if(PHYSX_LINUX_PRESENT AND EXISTS "${PHYSX_LINUX_STAMP}")
                file(READ "${PHYSX_LINUX_STAMP}" PHYSX_LINUX_STAMP_TEXT)
                string(STRIP "${PHYSX_LINUX_STAMP_TEXT}" PHYSX_LINUX_STAMP_TEXT)
                if(PHYSX_LINUX_STAMP_TEXT STREQUAL PHYSX_LINUX_SHA256)
                    set(PHYSX_LINUX_STAMP_OK ON)
                endif()
            endif()

            if(PHYSX_LINUX_STAMP_OK)
                set(N2ENGINE_PHYSX_AVAILABLE ON)
            else()
                message(STATUS "Downloading PhysX Linux binaries (${PHYSX_LINUX_ARCHIVE_NAME})...")
                # No EXPECTED_HASH: it makes a failed download or a mismatch a hard CMake error, and the plan is to
                # fall back to PhysX OFF. The hash is checked below, before anything is extracted.
                file(DOWNLOAD "${PHYSX_LINUX_URL}" "${PHYSX_LINUX_ARCHIVE}"
                        SHOW_PROGRESS STATUS DOWNLOAD_STATUS TIMEOUT 600)
                list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
                list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)
                set(PHYSX_LINUX_ARCHIVE_SHA256 "")
                if(STATUS_CODE EQUAL 0)
                    file(SHA256 "${PHYSX_LINUX_ARCHIVE}" PHYSX_LINUX_ARCHIVE_SHA256)
                endif()
                if(NOT STATUS_CODE EQUAL 0)
                    file(REMOVE "${PHYSX_LINUX_ARCHIVE}")
                    message(WARNING "Failed to download PhysX Linux binaries: ${STATUS_MESSAGE}. Disabling PhysX.")
                    set(N2ENGINE_USE_PHYSX OFF)
                elseif(NOT PHYSX_LINUX_ARCHIVE_SHA256 STREQUAL PHYSX_LINUX_SHA256)
                    file(REMOVE "${PHYSX_LINUX_ARCHIVE}")
                    message(WARNING "The PhysX Linux archive's SHA256 (${PHYSX_LINUX_ARCHIVE_SHA256}) does not match the pinned one (${PHYSX_LINUX_SHA256}). Disabling PhysX.")
                    set(N2ENGINE_USE_PHYSX OFF)
                else()
                    file(REMOVE_RECURSE "${PHYSX_LIB_DIR}")
                    file(ARCHIVE_EXTRACT INPUT "${PHYSX_LINUX_ARCHIVE}" DESTINATION "${PHYSX_ROOT}/physx")
                    file(REMOVE "${PHYSX_LINUX_ARCHIVE}")
                    # Keep the archive's licence next to the libraries (bin/ is ignored), not as an untracked file in the tree
                    if(EXISTS "${PHYSX_ROOT}/physx/LICENSE.md")
                        file(RENAME "${PHYSX_ROOT}/physx/LICENSE.md" "${PHYSX_LIB_DIR}/LICENSE.md")
                    endif()
                    set(PHYSX_LINUX_PRESENT ON)
                    foreach(file IN LISTS PHYSX_LINUX_LIB_FILES)
                        if(NOT EXISTS "${file}")
                            set(PHYSX_LINUX_PRESENT OFF)
                            message(WARNING "The PhysX Linux archive has no ${file}. Disabling PhysX.")
                        endif()
                    endforeach()
                    if(PHYSX_LINUX_PRESENT)
                        file(WRITE "${PHYSX_LINUX_STAMP}" "${PHYSX_LINUX_SHA256}\n")
                        set(N2ENGINE_PHYSX_AVAILABLE ON)
                    else()
                        set(N2ENGINE_USE_PHYSX OFF)
                    endif()
                endif()
            endif()

            if(N2ENGINE_PHYSX_AVAILABLE)
                # The libraries reference each other in cycles, so they go in one link group (full paths: the order
                # inside the group does not matter, the group and the system libraries after it do)
                set(PHYSX_LIBS -Wl,--start-group ${PHYSX_LINUX_LIB_FILES} -Wl,--end-group pthread dl rt)
                set(PHYSX_DLLS "")
            endif()
        endif()
    endif()
endif()

if(N2ENGINE_PHYSX_AVAILABLE)
    message(STATUS "PhysX enabled")
else()
    message(STATUS "PhysX disabled - no physics backend active")
endif()

file(GLOB LUA_SOURCES
        ${CMAKE_SOURCE_DIR}/external/lua/*.c
)
# Remove lua.c and luac.c standalone interpreter/compiler
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*lua\\.c$")
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*luac\\.c$")

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${CMAKE_SOURCE_DIR}/external/lua)

if(WIN32)
    set_target_properties(lua PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# == Production targets ==
add_subdirectory(math)
add_subdirectory(text)
add_subdirectory(assetimport)
add_subdirectory(renderer)
add_subdirectory(engine)
add_subdirectory(test_project)
add_subdirectory(lua_project)
add_subdirectory(editor-server)

if(TARGET test_project AND N2ENGINE_PHYSX_AVAILABLE)
    target_include_directories(test_project PRIVATE ${PHYSX_INCLUDE_DIR})
    target_link_directories(test_project PRIVATE ${PHYSX_LIB_DIR})
    target_link_libraries(test_project PRIVATE ${PHYSX_LIBS})
    # N2ENGINE_PHYSX_ENABLED comes from the engine target (PUBLIC)
endif()

add_library(dummy_target INTERFACE)
target_include_directories(dummy_target INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dummy_include)

# == Audio ==
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        OpenAL
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG 1.24.0
)
FetchContent_MakeAvailable(OpenAL)

# == Tests ==
if(N2ENGINE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()