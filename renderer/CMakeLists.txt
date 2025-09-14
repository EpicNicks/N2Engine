include(FetchContent)

# Help CMake find Vulkan on Windows if VULKAN_SDK isn't set
if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
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

# Only create library if we have source files
if(RENDERER_SOURCES)
    add_library(renderer STATIC ${RENDERER_SOURCES})
else()
    add_library(renderer INTERFACE)
endif()

# Find graphics APIs
find_package(Vulkan REQUIRED)
find_package(OpenGL REQUIRED)

# Configure GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)

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
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_XLIB_KHR")
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
        ${Vulkan_INCLUDE_DIRS}
        MathCore::math
)

# Compile definitions
target_compile_definitions(renderer PRIVATE 
    GLFW_INCLUDE_NONE
    GLFW_INCLUDE_VULKAN
    ${VULKAN_PLATFORM_DEFINE}
)

# Link libraries
target_link_libraries(renderer
    PUBLIC
        glad
        glfw
        OpenGL::GL
        Vulkan::Vulkan
)

# Set standard
set_target_properties(renderer PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
