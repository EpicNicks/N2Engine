file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# n2net (the netcode's packet layer and network simulator) has no engine dependency: it is its own library, below, and
# the engine does not link it yet. Keep its sources out of the engine's glob.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/network/n2net/")

add_library(engine STATIC
    ${ENGINE_SOURCES}
)

target_include_directories(engine
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/nlohmann/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/dr_libs/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/stb_vorbis/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/lua>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/sol2/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE
    src
)

set_target_properties(engine PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# The engine version (engine/Version.hpp): the root project's (engine declares no project of its own)
target_compile_definitions(engine PRIVATE N2ENGINE_VERSION="${PROJECT_VERSION}")

# The Lua allocator's fault hook (LuaRuntime.cpp): lets a test refuse the state's growth allocations from the Nth on, so
# a registry growth fails where the test wants it. Compiled in only when the unit tests are built; a shipped build
# (N2ENGINE_BUILD_TESTS=OFF) has neither the hook's functions nor its check in the allocator.
if (N2ENGINE_BUILD_TESTS)
    target_compile_definitions(engine PRIVATE N2ENGINE_LUA_ALLOC_FAULT_HOOK=1)
endif ()

# Debug define — works with multi-config generators (VS) too.
target_compile_definitions(engine PUBLIC
    $<$<CONFIG:Debug>:N2ENGINE_DEBUG=1>
)

# sol2 only type-checks Lua arguments in debug builds by default; without this, a wrong argument from a
# script (nil, a different usertype, `.` instead of `:`) is an unchecked cast in Release. PUBLIC so every
# target that includes sol through the engine agrees on sol's layout (a mismatch would be an ODR violation).
target_compile_definitions(engine PUBLIC SOL_ALL_SAFETIES_ON=1)

# PhysX is optional (N2ENGINE_USE_PHYSX, or the binaries failed to download). Without it the engine
# runs with no physics backend: components cope with a null backend and health reports it Disabled.
if (N2ENGINE_PHYSX_AVAILABLE)
    target_compile_definitions(engine PUBLIC N2ENGINE_PHYSX_ENABLED=1)
    target_include_directories(engine PUBLIC $<BUILD_INTERFACE:${PHYSX_INCLUDE_DIR}>)
    target_link_libraries(engine PUBLIC ${PHYSX_LIBS})
    if (N2ENGINE_PLATFORM_WEB)
        # The web libraries (cmake/PhysXWeb.cmake) are static, CPU-only, with PhysX's scalar math: the SIMD path needs
        # WebAssembly SIMD. Every target that includes the PhysX headers must see the same macro as the libraries
        # were built with (scripts/web-build-physx.sh), hence PUBLIC. Full library paths, no link directory.
        target_compile_definitions(engine PUBLIC PX_SIMD_DISABLED=1)
    elseif (WIN32)
        target_link_directories(engine PUBLIC
            ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<IF:$<CONFIG:Debug>,debug,release>
        )
    elseif (UNIX AND NOT APPLE)
        # Static Release-only libraries (full paths in PHYSX_LIBS, from the root CMakeLists.txt): every configuration
        # links them. They are the NDEBUG flavour; the PhysX headers do not require NDEBUG on Linux (that check is
        # Windows-only), and the engine never defines PX_CHECKED or PX_DEBUG.
        target_link_directories(engine PUBLIC ${PHYSX_LIB_DIR})
    endif ()
endif ()

# Jolt Physics is optional (N2ENGINE_USE_JOLT): the second 3D backend (engine/src/physics/jolt/JoltBackend.cpp). PUBLIC so a
# target that includes the engine headers sees N2ENGINE_JOLT_ENABLED (the engine's tests, the hosts) and every target
# that holds Jolt's inline math is compiled with the floating-point and instruction-set flags Jolt::Jolt carries
# (cmake/jolt/CMakeLists.txt). Without it the backend's sources compile to nothing.
if (N2ENGINE_USE_JOLT)
    target_compile_definitions(engine PUBLIC N2ENGINE_JOLT_ENABLED=1)
    target_link_libraries(engine PUBLIC Jolt::Jolt)
endif ()

if (WIN32)
    set_target_properties(engine PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(engine PRIVATE /bigobj)
endif ()

# Web: the standard library has no std::generator (external/web-stubs/include/generator stands in for it)
if (N2ENGINE_PLATFORM_WEB)
    target_link_libraries(engine PUBLIC n2_web_cxx_compat)
endif ()

target_link_libraries(engine
    PRIVATE
    renderer
    MathCore::math
    N2Engine::assetimport # image decoding for Texture assets; no assetimport type is in an engine header
    PUBLIC
    OpenAL::OpenAL # public audio headers include <AL/al.h>
    lua
    N2Engine::text # public: engine/text/Font.hpp exposes the text library's types
)

# == n2net: ITransport, the in-memory SimTransport, the packet layer (acks, reliable and unreliable channels,
# fragmentation) and, on native platforms only, UdpTransport. Everything but UdpTransport is standard library only: no
# sockets, no threads, no platform headers, no engine types, so it builds the same on Windows, Linux, arm64 and the
# single-threaded web build. UdpTransport.cpp uses BSD sockets or Winsock and compiles to nothing under Emscripten
# (N2NET_HAS_UDP is 0 there). Headers are under include/engine/network/n2net/.
file(GLOB N2NET_SOURCES "src/network/n2net/*.cpp")
add_library(n2net STATIC ${N2NET_SOURCES})
add_library(N2Engine::n2net ALIAS n2net)

target_include_directories(n2net
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

set_target_properties(n2net PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Winsock for UdpTransport (the web build has no UdpTransport and no Winsock)
if (WIN32)
    target_link_libraries(n2net PUBLIC ws2_32)
endif ()

if (MSVC)
    target_compile_options(n2net PRIVATE /W4)
else ()
    target_compile_options(n2net PRIVATE -Wall -Wextra)
endif ()
