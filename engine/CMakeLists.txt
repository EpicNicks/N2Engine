file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# n2net (the netcode's packet layer and network simulator) has no engine dependency: it is its own library, below, and
# the engine links it (the world layer in src/network/ runs n2net Connections). Keep its sources out of the engine's glob.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/network/n2net/")
# n2security (the shared admission policy, token files and crypto wrappers) is its own library too, below
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/security/")

# The particles and soft-body modules (N2ENGINE_USE_PARTICLES, N2ENGINE_USE_SOFTBODIES; docs/project-modules.html). A module
# is its simulation: the backends (CPU, WebGPU, and the PhysX GPU one with its driver), and for particles the emitter and the
# billboards, for soft bodies the tetrahedraliser. Off, those sources are not compiled at all, so nothing can still reach
# them. What stays is the component (ParticleSystem.cpp, SoftBody.cpp: fields, registration, Lua's methods), the backend
# selection and the usage counts of the health rows. The few files that name a module's code get its definition on
# themselves only (below), like Box2D's, so the headers and every other file are the same whatever the options.
set(N2ENGINE_PARTICLES_SOURCES
    src/particles/CpuParticleBackend.cpp
    src/particles/ParticleBillboards.cpp
    src/particles/ParticleEmitter.cpp
    src/particles/PbdParticleBackend.cpp
    src/particles/PbdPresets.cpp
    src/particles/WebGpuParticleBackend.cpp
    src/physics/physx/PhysXPbdDriver.cpp
)
set(N2ENGINE_SOFTBODIES_SOURCES
    src/physics/CpuSoftBodyBackend.cpp
    src/softbody/DeformableSoftBodyBackend.cpp
    src/softbody/Tetrahedraliser.cpp
    src/softbody/WebGpuSoftBodyBackend.cpp
    src/physics/physx/DeformableCooking.cpp
    src/physics/physx/PhysXDeformableDriver.cpp
)
foreach(_n2_module PARTICLES SOFTBODIES)
    foreach(_n2_source IN LISTS N2ENGINE_${_n2_module}_SOURCES)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_n2_source}")
            message(FATAL_ERROR "N2ENGINE_${_n2_module}_SOURCES names a file that does not exist: ${_n2_source}")
        endif()
        if(NOT N2ENGINE_USE_${_n2_module})
            list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_n2_source}")
        endif()
    endforeach()
    if(N2ENGINE_USE_${_n2_module})
        set_property(SOURCE src/rendering/ParticleSystem.cpp src/physics/SoftBody.cpp src/physics/physx/PhysXBackend.cpp
                     src/Application.cpp APPEND PROPERTY COMPILE_DEFINITIONS N2ENGINE_${_n2_module}_ENABLED=1)
    endif()
endforeach()

# The desktop platform backend and the GLFW input device are GLFW's only callers (build targets stage 6): not in the web
# build, which has no GLFW (its player runs on the headless backend)
if (N2ENGINE_PLATFORM_WEB OR N2ENGINE_PLATFORM_ANDROID)
    set(N2ENGINE_NO_GLFW ON)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/platform/DesktopGlfwHost\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/src/input/GlfwInputDevice\.cpp$")
endif ()

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

# Box2D is optional (N2ENGINE_USE_BOX2D): 2D physics, in engine/src/physics2d. Only Physics2DWorld.cpp names Box2D, so only it
# gets Box2D's headers and floating-point flags; LINK_ONLY keeps those flags off the rest of the engine, whose 3D physics and
# determinism numbers must not change. Without it that file compiles to a Create that returns null, and no Box2D code is in
# the binary (docs/physics2d.html#stripping).
if (N2ENGINE_USE_BOX2D)
    target_link_libraries(engine PRIVATE $<LINK_ONLY:box2d::box2d>)
    get_target_property(_n2_box2d_compile_options box2d INTERFACE_COMPILE_OPTIONS)
    set_source_files_properties(src/physics2d/Physics2DWorld.cpp PROPERTIES
        COMPILE_DEFINITIONS N2ENGINE_BOX2D_ENABLED=1
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/external/box2d/include"
        COMPILE_OPTIONS "${_n2_box2d_compile_options}"
    )
endif ()

if (WIN32)
    set_target_properties(engine PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(engine PRIVATE /bigobj)
    # GetProcessMemoryInfo (src/diagnostics/ProcessMemory.cpp): kernel32's K32 name on Windows 7 and later, psapi for the rest
    target_link_libraries(engine PRIVATE psapi)
endif ()

# Web and Android (libc++): the standard library has no std::generator (external/web-stubs/include/generator stands in)
if (N2ENGINE_PLATFORM_WEB OR N2ENGINE_PLATFORM_ANDROID)
    target_link_libraries(engine PUBLIC n2_web_cxx_compat)
endif ()
# No GLFW in this build (the web, Android): the platform layer's desktop backend is not compiled (HeadlessHost.cpp)
if (N2ENGINE_NO_GLFW)
    target_compile_definitions(engine PRIVATE N2ENGINE_NO_GLFW=1)
endif ()

target_link_libraries(engine
    PRIVATE
    renderer
    # The WGSL of the WebGPU particle and soft-body backends (header only, generated by renderer/CMakeLists.txt in every
    # build, so the backends compile everywhere; they run only on a renderer that offers compute)
    renderer_webgpu_shaders
    MathCore::math
    N2Engine::assetimport # image decoding for Texture assets; no assetimport type is in an engine header
    PUBLIC
    OpenAL::OpenAL # public audio headers include <AL/al.h>
    lua
    N2Engine::text # public: engine/text/Font.hpp exposes the text library's types
    n2net # public: the network headers (NetworkServer, NetworkClient) expose its Connection and ITransport
)

# == n2net: ITransport, the in-memory SimTransport, the packet layer (acks, reliable and unreliable channels,
# fragmentation), the join tokens and, on native platforms only, UdpTransport and SecureTransport (the handshake and the
# sealed packets). Everything but those two is standard library only (plus n2security, below): no sockets, no threads, no
# platform headers, no engine types, so it builds the same on Windows, Linux, arm64 and the single-threaded web build.
# UdpTransport.cpp uses BSD sockets or Winsock and compiles to nothing under Emscripten (N2NET_HAS_UDP is 0 there);
# SecureTransport.cpp needs n2security's crypto and compiles to nothing without it (N2NET_HAS_SECURE is 0 on the web
# build). Headers are under include/engine/network/n2net/.
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

# n2security: Security::Crypto (it wraps n2_mbedcrypto) and the admission policy SecureTransport uses
target_link_libraries(n2net PUBLIC n2security)
if (TARGET n2_mbedcrypto)
    target_link_libraries(n2net PRIVATE n2_mbedcrypto)
endif ()
if (EMSCRIPTEN)
    target_link_options(n2net PUBLIC "-lwebsocket.js")
endif ()

# Winsock for UdpTransport (the web build has no UdpTransport and no Winsock)
if (WIN32)
    target_link_libraries(n2net PUBLIC ws2_32)
endif ()

if (MSVC)
    target_compile_options(n2net PRIVATE /W4)
else ()
    target_compile_options(n2net PRIVATE -Wall -Wextra)
endif ()

# == n2security: the socket-free admission policy (PeerAddress, RateLimiter, TokenBucket, PreAuthSlots), the token files
# (TokenTable) shared by the editor host and the netcode, and Security::Crypto, thin wrappers over the vendored Mbed TLS's
# crypto modules (n2_mbedcrypto). Native builds link the crypto library and define N2SECURITY_HAS_CRYPTO=1; the web build
# has none (cmake/mbedtls is not added there), so there Crypto.cpp compiles to functions that report failure and
# N2SECURITY_HAS_CRYPTO is 0. No Mbed TLS type reaches a header here, so the library's include directories stay private.
file(GLOB N2SECURITY_SOURCES "src/security/*.cpp")
add_library(n2security STATIC ${N2SECURITY_SOURCES})
add_library(N2Engine::n2security ALIAS n2security)

target_include_directories(n2security
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
    ${CMAKE_SOURCE_DIR}/external/nlohmann/include
)

set_target_properties(n2security PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if (TARGET n2_mbedcrypto)
    target_link_libraries(n2security PRIVATE n2_mbedcrypto)
    target_compile_definitions(n2security PUBLIC N2SECURITY_HAS_CRYPTO=1)
else ()
    target_compile_definitions(n2security PUBLIC N2SECURITY_HAS_CRYPTO=0)
endif ()

if (WIN32)
    set_target_properties(n2security PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif ()

if (MSVC)
    target_compile_options(n2security PRIVATE /W4)
else ()
    target_compile_options(n2security PRIVATE -Wall -Wextra)
endif ()
