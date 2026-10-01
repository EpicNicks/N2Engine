file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

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
    if (WIN32)
        target_link_directories(engine PUBLIC
            ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/$<IF:$<CONFIG:Debug>,debug,release>
        )
    endif ()
endif ()

if (WIN32)
    set_target_properties(engine PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(engine PRIVATE /bigobj)
endif ()

target_link_libraries(engine
    PRIVATE
    renderer
    MathCore::math
    PUBLIC
    OpenAL::OpenAL # public audio headers include <AL/al.h>
    lua
)