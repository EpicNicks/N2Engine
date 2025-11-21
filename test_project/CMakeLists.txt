set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Export compile_commands.json for IDEs
find_package(OpenGL REQUIRED)

file(GLOB_RECURSE TEST_PROJECT_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

add_executable(test_project
    ${TEST_PROJECT_SOURCES}
)

target_include_directories(test_project 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(test_project PRIVATE
    renderer  # glfw, Vulkan, OpenGL, glad
    engine
    MathCore::math
)

if(WIN32)
    list(LENGTH PHYSX_DLLS DLL_COUNT)
    if(DLL_COUNT GREATER 0)
        message(STATUS "Copying ${DLL_COUNT} PhysX DLLs to test_project output")
        foreach(DLL IN LISTS PHYSX_DLLS)
            add_custom_command(TARGET test_project POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PHYSX_LIB_DIR}/${DLL}"
                    "$<TARGET_FILE_DIR:test_project>/${DLL}"
                COMMENT "Copying ${DLL}"
            )
        endforeach()
    else()
        message(WARNING "PHYSX_DLLS is empty - no DLLs will be copied!")
    endif()
endif()