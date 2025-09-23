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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(test_project PRIVATE N2ENGINE_DEBUG=1)
endif()

target_link_libraries(test_project PRIVATE
    renderer  # glfw, Vulkan, OpenGL, glad
    engine
    math
)