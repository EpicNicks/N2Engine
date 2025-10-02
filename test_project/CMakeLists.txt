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
    math
)