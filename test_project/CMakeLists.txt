find_package(OpenGL REQUIRED)

add_executable(test_project
    src/Main.cpp
)

target_link_libraries(test_project PRIVATE
    renderer  # glfw, Vulkan, OpenGL, glad
    engine
    math
)