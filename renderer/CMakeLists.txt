include(FetchContent)

# Help CMake find Vulkan on Windows if VULKAN_SDK isn't set
if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    # Try to find Vulkan SDK in common installation locations
    file(GLOB VULKAN_SDK_DIRS "C:/VulkanSDK/*")
    if(VULKAN_SDK_DIRS)
        list(GET VULKAN_SDK_DIRS -1 VULKAN_SDK_PATH) # Get the latest version
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
    # Create an interface library if no source files exist yet
    add_library(renderer INTERFACE)
endif()

find_package(Vulkan REQUIRED)

# Configure GLFW with Vulkan support
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)  # Use dynamic Vulkan linking

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

target_include_directories(renderer PRIVATE MathCore::math)

# Platform-specific defines
if(WIN32)
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_WIN32_KHR")
elseif(UNIX AND NOT APPLE)
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_XLIB_KHR")
elseif(APPLE)
    set(VULKAN_PLATFORM_DEFINE "VK_USE_PLATFORM_MACOS_MVK")
endif()

# Configure target after it's created
if(RENDERER_SOURCES)
    # Add compile definitions to ensure Vulkan support
    target_compile_definitions(renderer PRIVATE 
        GLFW_INCLUDE_VULKAN
        ${VULKAN_PLATFORM_DEFINE}
    )
    
    target_include_directories(renderer 
        PUBLIC 
            include
        PRIVATE
            src
            ${Vulkan_INCLUDE_DIRS}
    )
    
    target_link_libraries(renderer 
        PUBLIC 
            glfw 
            Vulkan::Vulkan  # Use modern CMake Vulkan target
    )
    
    # Set target properties
    set_target_properties(renderer PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
else()
    # For interface library, just set up the dependencies
    target_compile_definitions(renderer INTERFACE 
        GLFW_INCLUDE_VULKAN
        ${VULKAN_PLATFORM_DEFINE}
    )
    
    target_include_directories(renderer INTERFACE 
        include
        ${Vulkan_INCLUDE_DIRS}
    )
    
    target_link_libraries(renderer INTERFACE 
        glfw 
        Vulkan::Vulkan
    )
endif()