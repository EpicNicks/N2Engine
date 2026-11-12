# The Android platform (build targets stage 8; docs/android.html). Included by the root CMakeLists.txt when the NDK's
# toolchain is in use (ANDROID), which players/android's Gradle build does: it configures this repository with the NDK and
# the GameActivity prefab, and builds libn2game.so (players/android/native).
#
#   * no PhysX (decision D8: Jolt and Box2D on mobile), no Vulkan renderer yet (stage 12), no WebGPU;
#   * no GLFW and no desktop OpenGL library: the software renderer presents through the platform's window buffer
#     (Renderer::Common::Surface::presentPixels; the Android backend fills it from ANativeWindow);
#   * the standard library is the NDK's libc++, which has no std::generator: external/web-stubs/include/generator stands in,
#     as on the web;
#   * OpenAL Soft is built from source as on the desktop; on Android it plays through OpenSL ES.

set(N2ENGINE_USE_PHYSX OFF CACHE BOOL "Use PhysX physics backend" FORCE)
# Jolt is the 3D physics here (no PhysX): on unless the project's build.modules.physics3d says otherwise (N2EngineModules.cmake
# sets a plain variable then, which wins over this cache value)
set(N2ENGINE_USE_JOLT ON CACHE BOOL "Build the vendored Jolt Physics library and the engine's Jolt physics backend" FORCE)
set(N2ENGINE_RENDERER_VULKAN OFF CACHE BOOL "" FORCE)
set(N2ENGINE_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)
add_compile_definitions(N2ENGINE_PLATFORM_ANDROID=1)
message(STATUS "Android (${ANDROID_ABI}, API ${ANDROID_PLATFORM}): software renderer through the window buffer, Jolt and Box2D, OpenAL Soft on OpenSL ES; no PhysX, Vulkan, GLFW or editor tools")

# What the renderer and the engine link on the desktop, empty here: nothing includes GLFW, and glad loads GL at run time
add_library(glfw INTERFACE)
add_library(OpenGL::GL INTERFACE IMPORTED GLOBAL)

# libc++'s missing <generator>, the web build's stand-in
add_library(n2_web_cxx_compat INTERFACE)
target_include_directories(n2_web_cxx_compat SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/external/web-stubs/include")
