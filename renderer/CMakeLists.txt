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
    # The shader compilers are optional components: a Vulkan SDK or package without one must not fail the configure (the
    # backend then builds without its built-in shaders: see the SPIR-V step below)
    find_package(Vulkan REQUIRED OPTIONAL_COMPONENTS glslangValidator glslc)
endif()
# Web: no OpenGL library and no GLFW to build. external/web-stubs (added by the root CMakeLists.txt first) defines the
# `glfw` and `OpenGL::GL` targets the renderer links: GLFW's real headers with no-op functions, and nothing for GL
if(NOT N2ENGINE_PLATFORM_WEB)
find_package(OpenGL REQUIRED)
endif()

# Configure GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC OFF CACHE BOOL "" FORCE)
# Linux: X11 only for now (the CI image and the dev packages are X11; Wayland would need wayland-scanner and its protocols)
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()

if(NOT N2ENGINE_PLATFORM_WEB)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)
endif()

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

# ---------------------------------------------------------------------------------------------------------------------
# The Vulkan backend's built-in shaders: GLSL 450 under shaders/vulkan/, compiled to SPIR-V when the renderer is BUILT and
# embedded as generated headers (const uint32_t arrays in <build>/generated/vulkan_shaders/, included only by
# src/vulkan/VulkanDraw.cpp). Nothing generated is committed, and glslang/shaderc are build tools only: nothing of them is
# linked or shipped. Every edit of a .vert/.frag rebuilds its header (DEPENDS on the source).
#
# The compiler is glslangValidator (Vulkan SDK, apt glslang-tools) or else glslc (shaderc, in the Vulkan SDK), found through
# FindVulkan and then on PATH. With neither, the configure only WARNS: the build succeeds, N2ENGINE_VULKAN_HAS_SPIRV is 0
# and the Vulkan renderer draws nothing (VulkanRenderer::HasBuiltInShaders() is false; the tests skip their draw tests, or
# fail them under N2_REQUIRE_VULKAN=1).
# ---------------------------------------------------------------------------------------------------------------------
if(N2ENGINE_RENDERER_VULKAN)
    set(N2_SPIRV_COMPILER "")
    set(N2_SPIRV_COMPILER_KIND "")
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
        set(N2_SPIRV_COMPILER "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")
        set(N2_SPIRV_COMPILER_KIND "glslangValidator")
    elseif(Vulkan_GLSLC_EXECUTABLE)
        set(N2_SPIRV_COMPILER "${Vulkan_GLSLC_EXECUTABLE}")
        set(N2_SPIRV_COMPILER_KIND "glslc")
    else()
        # FindVulkan did not see one (an older CMake, or the tool outside the SDK): look on PATH and next to the SDK
        find_program(N2_GLSLANG_VALIDATOR_FALLBACK glslangValidator HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
        find_program(N2_GLSLC_FALLBACK glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
        if(N2_GLSLANG_VALIDATOR_FALLBACK)
            set(N2_SPIRV_COMPILER "${N2_GLSLANG_VALIDATOR_FALLBACK}")
            set(N2_SPIRV_COMPILER_KIND "glslangValidator")
        elseif(N2_GLSLC_FALLBACK)
            set(N2_SPIRV_COMPILER "${N2_GLSLC_FALLBACK}")
            set(N2_SPIRV_COMPILER_KIND "glslc")
        endif()
    endif()

    if(N2_SPIRV_COMPILER)
        # Every CI log shows which compiler built the shaders, and its version
        execute_process(COMMAND "${N2_SPIRV_COMPILER}" --version
                        OUTPUT_VARIABLE N2_SPIRV_COMPILER_VERSION ERROR_VARIABLE N2_SPIRV_COMPILER_VERSION_ERROR
                        RESULT_VARIABLE N2_SPIRV_COMPILER_VERSION_RESULT OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REPLACE "\n" "; " N2_SPIRV_COMPILER_VERSION "${N2_SPIRV_COMPILER_VERSION}")
        message(STATUS "Vulkan shader compiler: ${N2_SPIRV_COMPILER_KIND} at ${N2_SPIRV_COMPILER} (--version: ${N2_SPIRV_COMPILER_VERSION})")

        set(N2_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan")
        set(N2_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan_shaders")
        if(N2_SPIRV_COMPILER_KIND STREQUAL "glslc")
            # glslc -mfmt=c writes only the braced list of words: a one-line script turns it into the same header
            # glslangValidator --vn writes (a const uint32_t array)
            set(N2_SPIRV_WRAP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/n2_spirv_wrap.cmake")
            file(WRITE "${N2_SPIRV_WRAP_SCRIPT}" [=[
file(READ "${INPUT}" _words)
string(STRIP "${_words}" _words)
if(NOT _words MATCHES "^{")
  set(_words "{${_words}}")
endif()
file(WRITE "${OUTPUT}" "#pragma once\n#include <cstdint>\nconst uint32_t ${SYMBOL}[] =\n${_words};\n")
]=])
        endif()

        set(N2_SPIRV_HEADERS "")
        foreach(_shader unlit.vert unlit.frag lit.vert lit.frag text.vert text.frag)
            string(REPLACE "." "_" _id "${_shader}")
            set(_source "${N2_SHADER_SOURCE_DIR}/${_shader}")
            set(_header "${N2_SHADER_OUTPUT_DIR}/${_id}_spirv.h")
            if(N2_SPIRV_COMPILER_KIND STREQUAL "glslangValidator")
                add_custom_command(
                    OUTPUT "${_header}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${N2_SHADER_OUTPUT_DIR}"
                    COMMAND "${N2_SPIRV_COMPILER}" -V --target-env vulkan1.1 --vn "kSpirv_${_id}" -o "${_header}" "${_source}"
                    DEPENDS "${_source}"
                    COMMENT "Compiling ${_shader} to SPIR-V (glslangValidator)"
                    VERBATIM)
            else()
                add_custom_command(
                    OUTPUT "${_header}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${N2_SHADER_OUTPUT_DIR}"
                    COMMAND "${N2_SPIRV_COMPILER}" --target-env=vulkan1.1 -mfmt=c -o "${_header}.words" "${_source}"
                    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_header}.words" "-DOUTPUT=${_header}" "-DSYMBOL=kSpirv_${_id}"
                            -P "${N2_SPIRV_WRAP_SCRIPT}"
                    DEPENDS "${_source}"
                    COMMENT "Compiling ${_shader} to SPIR-V (glslc)"
                    VERBATIM)
            endif()
            list(APPEND N2_SPIRV_HEADERS "${_header}")
        endforeach()

        # Listed as sources of the renderer so building it runs the commands; PRIVATE, so only the renderer's own sources
        # (src/vulkan/VulkanDraw.cpp) can include them
        target_sources(renderer PRIVATE ${N2_SPIRV_HEADERS})
        target_include_directories(renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(renderer PRIVATE N2ENGINE_VULKAN_HAS_SPIRV=1)
    else()
        message(WARNING "Vulkan built-in shaders not compiled: no glslangValidator/glslc found (install glslang-tools, or the "
                        "Vulkan SDK with its shader tools); the Vulkan renderer will draw nothing")
        target_compile_definitions(renderer PRIVATE N2ENGINE_VULKAN_HAS_SPIRV=0)
    endif()
endif()

# ---------------------------------------------------------------------------------------------------------------------
# The WebGPU backend's built-in shaders (G1 of #155, design W1): hand-written WGSL under shaders/webgpu/, embedded as ONE
# generated header of string_views (<build>/generated/webgpu_shaders/renderer/webgpu/WgslSources.hpp, namespace
# Renderer::WebGpu::Wgsl: kUnlitWgsl, kLitWgsl, kTextWgsl) behind the header-only target renderer_webgpu_shaders. Written
# at configure time (no tool, so it is the same on native and web builds) and again whenever a .wgsl changes (they are
# CMAKE_CONFIGURE_DEPENDS); the file is rewritten only when its text changes, so an unchanged shader rebuilds nothing.
# Each shader goes in as raw string literals of at most 8000 characters, concatenated (MSVC limits a literal to 16380).
#
# Nothing uses it yet: there is no WebGPU backend and no N2ENGINE_RENDERER_WEBGPU option (stage G2 adds them and links this
# target); renderer_tests links it to check the embedded text is the files'. The shaders are validated by Tint and naga in CI
# (scripts/validate-wgsl.sh) and parsed by tests/renderer/webgpu/WebGpuShaderParityTests.cpp.
# ---------------------------------------------------------------------------------------------------------------------
set(N2_WGSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/webgpu")
set(N2_WGSL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/webgpu_shaders")
set(N2_WGSL_HEADER "${N2_WGSL_GENERATED_DIR}/renderer/webgpu/WgslSources.hpp")
set(N2_WGSL_CHUNK_SIZE 8000)
set(N2_WGSL_HEADER_TEXT "#pragma once\n\n#include <string_view>\n\n// Generated by renderer/CMakeLists.txt from renderer/shaders/webgpu/*.wgsl (the shaders with LF line ends): do not edit.\nnamespace Renderer::WebGpu::Wgsl\n{\n")
foreach(_entry unlit:Unlit lit:Lit text:Text)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _wgsl_name)
    list(GET _parts 1 _wgsl_symbol)
    set(_wgsl_file "${N2_WGSL_SOURCE_DIR}/${_wgsl_name}.wgsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wgsl_file}")
    file(READ "${_wgsl_file}" _wgsl)
    string(REPLACE "\r\n" "\n" _wgsl "${_wgsl}")
    string(LENGTH "${_wgsl}" _wgsl_length)
    string(APPEND N2_WGSL_HEADER_TEXT "    inline constexpr std::string_view k${_wgsl_symbol}Wgsl =")
    set(_wgsl_pos 0)
    while(_wgsl_pos LESS _wgsl_length)
        math(EXPR _wgsl_remaining "${_wgsl_length} - ${_wgsl_pos}")
        if(_wgsl_remaining GREATER N2_WGSL_CHUNK_SIZE)
            set(_wgsl_take ${N2_WGSL_CHUNK_SIZE})
        else()
            set(_wgsl_take ${_wgsl_remaining})
        endif()
        string(SUBSTRING "${_wgsl}" ${_wgsl_pos} ${_wgsl_take} _wgsl_chunk)
        string(APPEND N2_WGSL_HEADER_TEXT "\n        R\"n2wgsl(${_wgsl_chunk})n2wgsl\"")
        math(EXPR _wgsl_pos "${_wgsl_pos} + ${_wgsl_take}")
    endwhile()
    string(APPEND N2_WGSL_HEADER_TEXT ";\n\n")
endforeach()
string(APPEND N2_WGSL_HEADER_TEXT "}\n")

set(_wgsl_existing "")
if(EXISTS "${N2_WGSL_HEADER}")
    file(READ "${N2_WGSL_HEADER}" _wgsl_existing)
    # file(WRITE) writes CRLF on Windows: compare the texts with LF line ends, or every configure would rewrite the file
    string(REPLACE "\r\n" "\n" _wgsl_existing "${_wgsl_existing}")
endif()
if(NOT "${_wgsl_existing}" STREQUAL "${N2_WGSL_HEADER_TEXT}")
    file(WRITE "${N2_WGSL_HEADER}" "${N2_WGSL_HEADER_TEXT}")
endif()

add_library(renderer_webgpu_shaders INTERFACE)
target_include_directories(renderer_webgpu_shaders INTERFACE "${N2_WGSL_GENERATED_DIR}")

# Set standard
set_target_properties(renderer PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
