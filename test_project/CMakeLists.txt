find_package(OpenGL REQUIRED)

add_executable(test_project
    src/Main.cpp
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(test_project PRIVATE N2ENGINE_DEBUG=1)
endif()

target_link_libraries(test_project PRIVATE
    renderer  # glfw, Vulkan, OpenGL, glad
    engine
    math
)