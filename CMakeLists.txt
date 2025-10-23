cmake_minimum_required(VERSION 3.25)
project(N2Engine VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# PhysX Configuration (global, available to all subdirectories)
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
    )
    set(PHYSX_DLLS
        PhysX_64.dll
        PhysXCommon_64.dll
        PhysXFoundation_64.dll
        PhysXCooking_64.dll
    )
    
    if(EXISTS "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/release/PhysXGpu_64.dll")
        list(APPEND PHYSX_DLLS PhysXGpu_64.dll PhysXDevice64.dll)
        list(APPEND PHYSX_LIBS PhysXGpu_64)
    endif()
elseif(UNIX AND NOT APPLE)
    set(PHYSX_LIB_DIR ${PHYSX_ROOT}/physx/bin/linux.clang/$<LOWER_CASE:$<CONFIG>>)
    set(PHYSX_LIBS
        PhysX_static_64
        PhysXCommon_static_64
        PhysXFoundation_static_64
        PhysXCooking_static_64
        PhysXExtensions_static_64
    )
endif()

add_subdirectory(math)
add_subdirectory(renderer)
add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(test_project)

target_include_directories(test_project PRIVATE ${PHYSX_INCLUDE_DIR})
target_link_directories(test_project PRIVATE ${PHYSX_LIB_DIR})
target_link_libraries(test_project PRIVATE ${PHYSX_LIBS})