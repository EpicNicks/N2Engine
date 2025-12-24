cmake_minimum_required(VERSION 3.25)
project(N2Engine VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(N2ENGINE_BUILD_TESTS "Build unit tests" ON)
option(N2ENGINE_USE_PHYSX "Use PhysX physics backend" ON)

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
        set(PHYSX_ZIP "${CMAKE_BINARY_DIR}/PhysX_bins.zip")
        set(PHYSX_DOWNLOAD_URL "https://github.com/EpicNicks/N2Engine/releases/download/physx-dependencies/bin.zip")
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

        if(NOT EXISTS "${PHYSX_BIN_DIR}/win.x86_64.vc143.mt/debug/PhysX_64.dll")
            message(STATUS "Downloading PhysX binaries...")
            file(DOWNLOAD "${PHYSX_DOWNLOAD_URL}" "${PHYSX_ZIP}" SHOW_PROGRESS STATUS DOWNLOAD_STATUS TIMEOUT 600)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)
            if(NOT STATUS_CODE EQUAL 0)
                message(WARNING "Failed to download PhysX binaries: ${STATUS_MESSAGE}. Disabling PhysX.")
                set(N2ENGINE_USE_PHYSX OFF)
            else()
                file(ARCHIVE_EXTRACT INPUT "${PHYSX_ZIP}" DESTINATION "${PHYSX_ROOT}/physx")
                file(REMOVE "${PHYSX_ZIP}")
                set(N2ENGINE_PHYSX_AVAILABLE ON)
            endif()
        else()
            set(N2ENGINE_PHYSX_AVAILABLE ON)
        endif()

        if(N2ENGINE_PHYSX_AVAILABLE AND EXISTS "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/debug/PhysXGpu_64.dll")
            list(APPEND PHYSX_LIBS PhysXGpu_64)
            list(APPEND PHYSX_DLLS PhysXGpu_64.dll)
            message(STATUS "PhysX GPU support enabled")
        endif()
    elseif(UNIX AND NOT APPLE)
        message(WARNING "Linux PhysX binaries not yet configured. Disabling PhysX.")
        set(N2ENGINE_USE_PHYSX OFF)
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
add_subdirectory(renderer)
add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(test_project)

if(TARGET test_project AND N2ENGINE_PHYSX_AVAILABLE)
    target_include_directories(test_project PRIVATE ${PHYSX_INCLUDE_DIR})
    target_link_directories(test_project PRIVATE ${PHYSX_LIB_DIR})
    target_link_libraries(test_project PRIVATE ${PHYSX_LIBS})
    target_compile_definitions(test_project PRIVATE N2ENGINE_HAS_PHYSX=1)
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