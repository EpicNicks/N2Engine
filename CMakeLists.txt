cmake_minimum_required(VERSION 3.25)
project(N2Engine VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# == PhysX Binary Download ==
set(PHYSX_ROOT "${CMAKE_SOURCE_DIR}/external/PhysX")
set(PHYSX_BIN_DIR "${PHYSX_ROOT}/physx/bin")
set(PHYSX_ZIP "${CMAKE_BINARY_DIR}/PhysX_bins.zip")
set(PHYSX_DOWNLOAD_URL "https://github.com/EpicNicks/N2Engine/releases/download/physx-dependencies/bin.zip")

# Check if PhysX binaries exist
if(NOT EXISTS "${PHYSX_BIN_DIR}/win.x86_64.vc143.mt/debug/PhysX_64.dll")
    message(STATUS "========================================")
    message(STATUS "PhysX binaries not found")
    message(STATUS "Downloading from GitHub Releases...")
    message(STATUS "========================================")

    file(DOWNLOAD
            "${PHYSX_DOWNLOAD_URL}"
            "${PHYSX_ZIP}"
            SHOW_PROGRESS
            STATUS DOWNLOAD_STATUS
            TIMEOUT 600
    )

    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)

    if(NOT STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download PhysX binaries: ${STATUS_MESSAGE}\nManual download: ${PHYSX_DOWNLOAD_URL}")
    endif()

    message(STATUS "Extracting PhysX binaries...")
    file(ARCHIVE_EXTRACT
            INPUT "${PHYSX_ZIP}"
            DESTINATION "${PHYSX_ROOT}/physx"
    )

    file(REMOVE "${PHYSX_ZIP}")
    message(STATUS "PhysX binaries installed successfully!")
    message(STATUS "========================================")
else()
    message(STATUS "PhysX binaries found at ${PHYSX_BIN_DIR}")
endif()

# == PhysX Configuration (global, available to all subdirectories) ==
set(PHYSX_ROOT "${CMAKE_SOURCE_DIR}/external/PhysX")
set(PHYSX_INCLUDE_DIR ${PHYSX_ROOT}/physx/include)

if(WIN32)
    set(PHYSX_LIB_DIR ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>)
    
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
    
    # Check for GPU DLL (at configure time, check release folder)
    # Only add if it actually exists in your build
    if(EXISTS "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/debug/PhysXGpu_64.dll")
        message(STATUS "Found PhysXGpu_64.dll - enabling GPU support")
        list(APPEND PHYSX_DLLS PhysXGpu_64.dll)
        list(APPEND PHYSX_LIBS PhysXGpu_64)
    else()
        message(STATUS "PhysXGpu_64.dll not found - GPU support disabled")
    endif()
endif()

add_subdirectory(math)
add_subdirectory(renderer)
add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(test_project)

target_include_directories(test_project PRIVATE ${PHYSX_INCLUDE_DIR})
target_link_directories(test_project PRIVATE ${PHYSX_LIB_DIR})
target_link_libraries(test_project PRIVATE ${PHYSX_LIBS})
