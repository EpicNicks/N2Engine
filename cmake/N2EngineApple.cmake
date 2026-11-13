# Apple platforms (build targets stage 10; docs/apple.html). Included by the root CMakeLists.txt on macOS and iOS.
#
#   * macOS: the desktop player, as on Windows and Linux (GLFW, OpenGL and the software renderer), with AppleClang;
#   * iOS (CMAKE_SYSTEM_NAME iOS, Xcode generator): players/ios, a UIKit app whose software renderer's frame is shown as its
#     view's layer contents. No GLFW, no desktop OpenGL library, no editor tools or tests;
#   * the oldest systems: iOS 16.3 and macOS 13.3 (the targets' CMAKE_OSX_DEPLOYMENT_TARGET), where Apple's libc++ first has
#     floating-point std::to_chars, which std::format uses;
#   * macOS's GPU renderer is the Vulkan renderer on MoltenVK when MoltenVK is found (stage 13, below); iOS has the software
#     renderer until MoltenVK's iOS framework is in the build (a stage 13 follow-up);
#   * both: no PhysX (decision D8 for iOS; no PhysX build for macOS is vendored), no WebGPU. Apple's libc++ has no std::generator: external/web-stubs/include/generator
#     stands in, as on the web and Android.

set(N2ENGINE_USE_PHYSX OFF CACHE BOOL "Use PhysX physics backend" FORCE)
# GPU rendering on macOS is the Vulkan renderer on MoltenVK (build targets stage 13, the user's pick;
# orchestrator/proposals/metal-spike.md). On when MoltenVK is found; without it the player has the OpenGL and software
# renderers as before. The player links MoltenVK itself (libMoltenVK.dylib, shipped beside it): no Vulkan loader to install.
# N2ENGINE_APPLE_VULKAN_LOADER links the Vulkan loader instead, for test runs that need its layers (validation)
option(N2ENGINE_APPLE_MOLTENVK "macOS: the Vulkan renderer on MoltenVK" ON)
option(N2ENGINE_APPLE_VULKAN_LOADER "macOS: link the Vulkan loader (and its layers) rather than MoltenVK itself" OFF)
set(N2ENGINE_APPLE_MOLTENVK_LINKED OFF)
if(N2ENGINE_APPLE_MOLTENVK AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(N2ENGINE_APPLE_VULKAN_LOADER)
        set(N2ENGINE_RENDERER_VULKAN ON CACHE BOOL "" FORCE)
        message(STATUS "macOS: the Vulkan renderer through the Vulkan loader (N2ENGINE_APPLE_VULKAN_LOADER)")
    else()
        find_library(N2_MOLTENVK_LIBRARY MoltenVK HINTS /opt/homebrew/lib /usr/local/lib "$ENV{VULKAN_SDK}/lib")
        find_path(N2_MOLTENVK_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include "$ENV{VULKAN_SDK}/include")
        if(N2_MOLTENVK_LIBRARY AND N2_MOLTENVK_INCLUDE)
            # MoltenVK exports the Vulkan API itself: it stands in for the loader as Vulkan::Vulkan (FindVulkan then takes
            # these and makes no target of its own)
            add_library(Vulkan::Vulkan SHARED IMPORTED GLOBAL)
            set_target_properties(Vulkan::Vulkan PROPERTIES IMPORTED_LOCATION "${N2_MOLTENVK_LIBRARY}"
                                                            INTERFACE_INCLUDE_DIRECTORIES "${N2_MOLTENVK_INCLUDE}")
            set(Vulkan_LIBRARY "${N2_MOLTENVK_LIBRARY}" CACHE FILEPATH "MoltenVK, linked as the Vulkan library" FORCE)
            set(Vulkan_INCLUDE_DIR "${N2_MOLTENVK_INCLUDE}" CACHE PATH "" FORCE)
            set(N2ENGINE_RENDERER_VULKAN ON CACHE BOOL "" FORCE)
            set(N2ENGINE_APPLE_MOLTENVK_LINKED ON)
            add_compile_definitions(N2ENGINE_APPLE_MOLTENVK=1)
            message(STATUS "macOS: the Vulkan renderer on MoltenVK (${N2_MOLTENVK_LIBRARY})")
        else()
            set(N2ENGINE_RENDERER_VULKAN OFF CACHE BOOL "" FORCE)
            message(STATUS "macOS: MoltenVK not found (brew install molten-vk, or the Vulkan SDK): no Vulkan renderer")
        endif()
    endif()
else()
    set(N2ENGINE_RENDERER_VULKAN OFF CACHE BOOL "" FORCE)
endif()
# Jolt is the 3D physics here (no PhysX): on unless the project's build.modules.physics3d says otherwise (N2EngineModules.cmake
# sets a plain variable then, which wins over this cache value)
set(N2ENGINE_USE_JOLT ON CACHE BOOL "Build the vendored Jolt Physics library and the engine's Jolt physics backend" FORCE)
set(N2ENGINE_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)

# libc++'s missing <generator>, the web build's stand-in
add_library(n2_web_cxx_compat INTERFACE)
target_include_directories(n2_web_cxx_compat SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/external/web-stubs/include")

if(N2ENGINE_PLATFORM_IOS)
    add_compile_definitions(N2ENGINE_PLATFORM_IOS=1)
    # The math library picks its SIMD flags by processor; a cross build for the device or an Apple-silicon simulator is arm64
    if(CMAKE_OSX_ARCHITECTURES MATCHES "^arm64$" OR NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_SYSTEM_PROCESSOR arm64)
    elseif(CMAKE_OSX_ARCHITECTURES MATCHES "^x86_64$")
        set(CMAKE_SYSTEM_PROCESSOR x86_64)
    endif()
    # Apps are not signed in this build: the simulator runs unsigned apps, and a device build is signed by the workflow
    # with its repository secrets (docs/apple.html#signing)
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
    set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "YES")
    # What the renderer and the engine link on the desktop, empty here (as on Android)
    add_library(glfw INTERFACE)
    add_library(OpenGL::GL INTERFACE IMPORTED GLOBAL)
    message(STATUS "iOS (${CMAKE_OSX_SYSROOT}, ${CMAKE_SYSTEM_PROCESSOR}): software renderer into the view's layer, Jolt and Box2D, OpenAL Soft on Core Audio; no PhysX, Vulkan, GLFW, editor tools or tests")
else()
    add_compile_definitions(N2ENGINE_PLATFORM_MACOS=1)
    # OpenGL is deprecated on macOS, and works: silence the warning on every use
    add_compile_definitions(GL_SILENCE_DEPRECATION=1)
    message(STATUS "macOS: the desktop player (GLFW; OpenGL, software, and Vulkan on MoltenVK when found), Jolt and Box2D, OpenAL Soft on Core Audio; no PhysX or WebGPU")
endif()
