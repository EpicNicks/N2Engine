# The Android platform (build targets stage 8; docs/android.html). Included by the root CMakeLists.txt when the NDK's
# toolchain is in use (ANDROID), which players/android's Gradle build does: it configures this repository with the NDK and
# the GameActivity prefab, and builds libn2game.so (players/android/native).
#
#   * no PhysX (decision D8: Jolt and Box2D on mobile), no WebGPU;
#   * the Vulkan renderer only with N2ENGINE_ANDROID_VULKAN (build.targets.android.renderer "vulkan"; stage 12): the
#     NDK's own Vulkan headers and libvulkan.so, and its glslc for the built-in shaders. The player still falls back to the
#     software renderer on a device without Vulkan;
#   * no GLFW and no desktop OpenGL library: the software renderer presents through the platform's window buffer
#     (Renderer::Common::Surface::presentPixels; the Android backend fills it from ANativeWindow);
#   * the standard library is the NDK's libc++, which has no std::generator: external/web-stubs/include/generator stands in,
#     as on the web;
#   * OpenAL Soft is built from source as on the desktop; on Android it plays through OpenSL ES.

option(N2ENGINE_ANDROID_VULKAN "Android: build the Vulkan renderer (build.targets.android.renderer = vulkan)" OFF)

set(N2ENGINE_USE_PHYSX OFF CACHE BOOL "Use PhysX physics backend" FORCE)
set(N2ENGINE_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)
if(N2ENGINE_ANDROID_VULKAN)
    set(N2ENGINE_RENDERER_VULKAN ON CACHE BOOL "" FORCE)
    # The NDK's glslc (shader-tools/<host>/glslc), for renderer/CMakeLists.txt's SPIR-V step
    file(GLOB _n2_ndk_glslc "${ANDROID_NDK}/shader-tools/*/glslc" "${ANDROID_NDK}/shader-tools/*/glslc.exe")
    if(_n2_ndk_glslc)
        list(GET _n2_ndk_glslc 0 _n2_ndk_glslc)
        set(Vulkan_GLSLC_EXECUTABLE "${_n2_ndk_glslc}" CACHE FILEPATH "The NDK's glslc" FORCE)
    endif()
    # The NDK sysroot's Vulkan headers and loader (FindVulkan looks in the find root, which the NDK's toolchain sets)
    find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h)
    find_library(Vulkan_LIBRARY vulkan)
    add_compile_definitions(N2ENGINE_ANDROID_VULKAN=1)
    set(_n2_android_renderers "the Vulkan renderer (software where the device has no Vulkan)")
else()
    set(N2ENGINE_RENDERER_VULKAN OFF CACHE BOOL "" FORCE)
    set(_n2_android_renderers "the software renderer through the window buffer")
endif()
add_compile_definitions(N2ENGINE_PLATFORM_ANDROID=1)
message(STATUS "Android (${ANDROID_ABI}, API ${ANDROID_PLATFORM}): ${_n2_android_renderers}, Jolt and Box2D, OpenAL Soft on OpenSL ES; no PhysX, GLFW or editor tools")

# What the renderer and the engine link on the desktop, empty here: nothing includes GLFW, and glad loads GL at run time
add_library(glfw INTERFACE)
add_library(OpenGL::GL INTERFACE IMPORTED GLOBAL)

# libc++'s missing <generator>, the web build's stand-in
add_library(n2_web_cxx_compat INTERFACE)
target_include_directories(n2_web_cxx_compat SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/external/web-stubs/include")
