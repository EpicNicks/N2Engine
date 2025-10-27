# Collect all source files
file(GLOB_RECURSE EDITOR_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Create the static library
add_executable(editor
    ${EDITOR_SOURCES}
)

# Set include directories
target_include_directories(editor 
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/nlohmann/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set target properties
set_target_properties(editor PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Add any necessary libraries
target_link_libraries(editor PRIVATE 
    engine 
    renderer 
    MathCore::math
)

if (NOT MSVC)
    # experimental C++23 features for G++
    target_link_libraries(editor PRIVATE "-lstdc++exp")
endif()

if(WIN32)
    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>/PhysX_64.dll"
            "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>/PhysXCommon_64.dll"
            "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>/PhysXFoundation_64.dll"
            "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<LOWER_CASE:$<CONFIG>>/PhysXCooking_64.dll"
            $<TARGET_FILE_DIR:editor>
        COMMENT "Copying PhysX DLLs to editor directory"
    )
endif()