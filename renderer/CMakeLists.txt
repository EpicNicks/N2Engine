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
target_include_directories(your_target PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(your_target PRIVATE ${Vulkan_LIBRARIES})