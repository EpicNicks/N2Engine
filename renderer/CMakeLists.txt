include(FetchContent)

# Help CMake find Vulkan on Windows if VULKAN_SDK isn't set
if(N2ENGINE_RENDERER_VULKAN AND WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB VULKAN_SDK_DIRS "C:/VulkanSDK/*")
    if(VULKAN_SDK_DIRS)
        list(GET VULKAN_SDK_DIRS -1 VULKAN_SDK_PATH)
        set(ENV{VULKAN_SDK} ${VULKAN_SDK_PATH})
        message(STATUS "Found Vulkan SDK at: ${VULKAN_SDK_PATH}")
    endif()
endif()

# Automatically find all source files in the renderer directory
file(GLOB_RECURSE RENDERER_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Without the Vulkan backend its sources are not built (and nothing includes its headers: see RendererConfig.hpp)
if(NOT N2ENGINE_RENDERER_VULKAN)
    list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/src/vulkan/")
endif()

# Only create library if we have source files
if(RENDERER_SOURCES)
    add_library(renderer STATIC ${RENDERER_SOURCES})
else()
    add_library(renderer INTERFACE)
endif()

# Find graphics APIs
if(N2ENGINE_RENDERER_VULKAN)
    find_package(Vulkan REQUIRED)
endif()
find_package(OpenGL REQUIRED)

# Configure GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)
# Linux: X11 only for now (the CI image and the dev packages are X11; Wayland would need wayland-scanner and its protocols)
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

# Add GLAD
add_library(glad ${CMAKE_SOURCE_DIR}/external/glad/src/glad.c)
target_include_directories(glad PUBLIC ${CMAKE_SOURCE_DIR}/external/glad/include)

# Platform-specific defines
if(WIN32)
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_WIN32_KHR")
elseif(UNIX AND NOT APPLE)
    # No platform define: the surface comes from glfwCreateWindowSurface, and VK_USE_PLATFORM_XLIB_KHR would pull
    # <X11/Xlib.h> into vulkan.h, whose macros (None, Bool, Status, ...) break the engine's own names (CullMode::None)
    set(VULKAN_PLATFORM_DEFINE "")
elseif(APPLE)
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_MACOS_MVK")
endif()

# Include GLAD in renderer target so headers can find it
target_include_directories(renderer
    PUBLIC
    include
        ${CMAKE_SOURCE_DIR}/external/glad/include
    PRIVATE
    src
        MathCore::math
)
if(N2ENGINE_RENDERER_VULKAN)
    target_include_directories(renderer PRIVATE ${Vulkan_INCLUDE_DIRS})
    # AMD VulkanMemoryAllocator (MIT, one vendored header): PRIVATE, so no public header and no dependant sees it, and
    # SYSTEM, so its own warnings stay out of our build. Only renderer/src/vulkan/ includes it (VulkanVma.hpp)
    target_include_directories(renderer SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator)
endif()

# Compile definitions
target_compile_definitions(renderer PRIVATE 
    GLFW_INCLUDE_NONE
    NOMINMAX # remove windows min and max macros from polluting the <algorithm> function
)
if(N2ENGINE_RENDERER_VULKAN)
    target_compile_definitions(renderer PRIVATE
        GLFW_INCLUDE_VULKAN
        ${VULKAN_PLATFORM_DEFINE}
    )
endif()
# 1 or 0 for everything that links the renderer (renderer/common/RendererConfig.hpp)
target_compile_definitions(renderer PUBLIC N2ENGINE_RENDERER_VULKAN=$<BOOL:${N2ENGINE_RENDERER_VULKAN}>)

# Link libraries
target_link_libraries(renderer
    PUBLIC
        glad
        glfw
        OpenGL::GL
        MathCore::math
)
if(N2ENGINE_RENDERER_VULKAN)
    target_link_libraries(renderer PUBLIC Vulkan::Vulkan)
endif()

# Set standard
set_target_properties(renderer PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
