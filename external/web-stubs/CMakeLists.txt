# Web (Emscripten) only: stand-ins for the two native libraries the engine's public headers include, so the engine core
# builds and runs headless without a window or an audio device (stage 1 of issue #155; docs/web-build.html).
#
#   glfw           GLFW 3.4's real headers (so every type and constant is identical to the native build) and a no-op
#                  implementation of the functions the engine calls: glfwInit fails, nothing creates a window. The
#                  engine already has a no-window path (Window::UsesNoWindow) that never calls GLFW; this only gives
#                  the linker the symbols. Stage 2 replaces it with a real window and input.
#   OpenAL::OpenAL OpenAL Soft 1.24's real headers and, by N2ENGINE_WEB_AUDIO (cmake/N2EngineWeb.cmake): webaudio (the
#                  default, stage 4) Emscripten's own OpenAL port on WebAudio, or stub, a no-op implementation of the
#                  functions the engine calls (openal_stub.c): no device opens and no extension is present, so the audio
#                  system reports itself disabled, as it does on a machine with no sound card.
#   OpenGL::GL     an empty interface target (the web has no OpenGL library to link; glad's loader is never called).
#   n2_web_cxx_compat  an include directory with a <generator> header: libc++ (Emscripten) has no std::generator yet,
#                  and the engine's coroutines are std::generator<ICoroutineWait>. The header defers to the toolchain's
#                  own <generator> when there is one. The engine links this target on the web.
#
# The headers are downloaded at configure time (archives of the same tags the native build uses) and only their include
# directories are used: SOURCE_SUBDIR names a directory that does not exist, so FetchContent does not add the other
# projects' own CMakeLists.txt.
include(FetchContent)

FetchContent_Declare(
    n2_glfw_headers
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
    SOURCE_SUBDIR n2-headers-only
)
FetchContent_Declare(
    n2_openal_headers
    URL https://github.com/kcat/openal-soft/archive/refs/tags/1.24.0.zip
    SOURCE_SUBDIR n2-headers-only
)
FetchContent_MakeAvailable(n2_glfw_headers n2_openal_headers)

add_library(glfw STATIC glfw_stub.c)
target_include_directories(glfw SYSTEM PUBLIC "${n2_glfw_headers_SOURCE_DIR}/include")

if(N2ENGINE_WEB_AUDIO STREQUAL "webaudio")
    # The headers are OpenAL Soft's (the engine's AudioSystem.hpp includes <AL/alext.h> for the loopback types, which
    # Emscripten's own AL headers lack); the implementation is Emscripten's port (library_openal.js on WebAudio), linked
    # by name: emcc turns -lopenal into that JavaScript library. Nothing of OpenAL Soft's code is built.
    add_library(openal_web INTERFACE)
    target_include_directories(openal_web SYSTEM INTERFACE "${n2_openal_headers_SOURCE_DIR}/include")
    target_link_libraries(openal_web INTERFACE openal)
else()
    add_library(openal_web STATIC openal_stub.c)
    target_include_directories(openal_web SYSTEM PUBLIC "${n2_openal_headers_SOURCE_DIR}/include")
endif()
add_library(OpenAL::OpenAL ALIAS openal_web)

add_library(OpenGL::GL INTERFACE IMPORTED GLOBAL)

add_library(n2_web_cxx_compat INTERFACE)
target_include_directories(n2_web_cxx_compat SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
