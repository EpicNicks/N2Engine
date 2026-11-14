# A native build without the audio module (N2ENGINE_USE_AUDIO=OFF; docs/project-modules.html): the no-op OpenAL of the web's
# N2ENGINE_WEB_AUDIO=stub (external/web-stubs/openal_stub.c) in place of OpenAL Soft. No device opens and no extension is
# present, so nothing could play even if the engine asked; the engine does not ask (AudioSystem::IsInBuild is false).
#
# The headers are OpenAL Soft 1.24.0's own (AudioSystem.hpp includes <AL/alext.h> for the loopback types), downloaded as an
# archive of the tag the full build fetches, and only their include directory is used: SOURCE_SUBDIR names a directory that
# does not exist, so OpenAL Soft's own CMakeLists.txt is not added and nothing of it is built.
#
# The library is named OpenAL, with the alias OpenAL::OpenAL, as OpenAL Soft's are: the engine links the alias and the desktop
# player asks the OpenAL target whether it is a shared library to ship (players/desktop/CMakeLists.txt). This one is static, so
# a package ships no OpenAL at all.
include(FetchContent)

FetchContent_Declare(
    n2_openal_stub_headers
    URL https://github.com/kcat/openal-soft/archive/refs/tags/1.24.0.zip
    SOURCE_SUBDIR n2-headers-only
)
FetchContent_MakeAvailable(n2_openal_stub_headers)

add_library(OpenAL STATIC "${CMAKE_SOURCE_DIR}/external/web-stubs/openal_stub.c")
target_include_directories(OpenAL SYSTEM PUBLIC "${n2_openal_stub_headers_SOURCE_DIR}/include")
# OpenAL Soft's headers declare every function __declspec(dllimport) on Windows unless the library is a static one
target_compile_definitions(OpenAL PUBLIC AL_LIBTYPE_STATIC)
if(WIN32)
    set_target_properties(OpenAL PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_library(OpenAL::OpenAL ALIAS OpenAL)
message(STATUS "Audio: not in this build (N2ENGINE_USE_AUDIO is OFF): a no-op OpenAL stands in for OpenAL Soft")
