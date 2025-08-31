target_include_directories(renderer PUBLIC include)

set(RENDERER_SOURCES
    # Public interface
    src/renderer.cpp
    src/buffer.cpp
    src/texture.cpp
    src/shader.cpp
    src/render_pass.cpp
    
    # Vulkan implementation
    src/vulkan/vulkan_renderer.cpp
    src/vulkan/vulkan_buffer.cpp
    src/vulkan/vulkan_texture.cpp
    src/vulkan/vulkan_shader.cpp
    src/vulkan/vulkan_device.cpp
    src/vulkan/vulkan_swapchain.cpp
    src/vulkan/vulkan_command_buffer.cpp
    src/vulkan/vulkan_memory_allocator.cpp
    
    # Common utilities
    src/common/resource_manager.cpp
    src/common/validation.cpp
)

add_library(renderer STATIC ${RENDERER_SOURCES})

find_package(Vulkan REQUIRED)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
)
FetchContent_MakeAvailable(glfw)

target_include_directories(renderer PRIVATE ${Vulkan_INCLUDE_DIRS} glfw)
target_link_libraries(renderer PRIVATE ${Vulkan_LIBRARIES})