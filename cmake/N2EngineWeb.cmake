# Web (Emscripten) platform support. Included from the root CMakeLists.txt only when N2ENGINE_PLATFORM_WEB is ON, which
# is derived from the toolchain (an Emscripten.cmake toolchain file, i.e. `emcmake cmake`), never set by hand. Native
# configurations never include this file.
#
# What this file does:
#   * defines the N2ENGINE_PLATFORM_WEB compile definition for every target, and N2ENGINE_SINGLE_THREADED (the baseline
#     build has no threads: the software renderer renders on the calling thread, see RenderThread.hpp)
#   * compiles and links everything with WebAssembly exceptions (the engine, sol2, nlohmann::json and googletest throw
#     and catch; Emscripten's default is to abort on a throw)
#   * defines the N2ENGINE_WEB_PROFILE cache variable (dev | size | speed) and turns it into compile and link flags
#   * defines N2ENGINE_WEB_PRECOMPRESS (the .br/.gz siblings of the web package) and a `web-size-report` target that runs
#     scripts/web-size-report.sh (size budgets are not here: they are in web/budgets.json, checked by
#     scripts/web-budget-check.mjs)
#
# The flags are global (add_compile_options / add_link_options) so that every library and executable, including the
# fetched googletest, is built the same way; LTO in particular needs every object to be compiled with it.

set(N2ENGINE_WEB_PROFILE "dev" CACHE STRING "Web build profile: dev (fast build), size (smallest output) or speed (fastest code)")
set_property(CACHE N2ENGINE_WEB_PROFILE PROPERTY STRINGS dev size speed)

if(NOT N2ENGINE_WEB_PROFILE MATCHES "^(dev|size|speed)$")
    message(FATAL_ERROR "N2ENGINE_WEB_PROFILE must be dev, size or speed (got '${N2ENGINE_WEB_PROFILE}')")
endif()

add_compile_definitions(N2ENGINE_PLATFORM_WEB=1)

# The baseline web build is single-threaded (no pthreads, no SharedArrayBuffer, no cross-origin isolation). Code that
# would start or wait on a thread checks this macro. A later threaded second artifact (stage 6) would not define it.
add_compile_definitions(N2ENGINE_SINGLE_THREADED=1)

# WebAssembly exceptions (the native wasm EH instructions, no JavaScript glue), for every language: they also make
# Lua's setjmp/longjmp (a C library) use the wasm implementation, so the C and C++ objects agree. Compile AND link flag.
add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

# Every profile: the heap may grow (the Emscripten default is a fixed 16 MB heap, which a game or a large test exceeds),
# and the stack is 1 MB (the Emscripten default of 64 KB is below what the engine's tests and recursive code expect).
# Nothing here needs cross-origin isolation, SharedArrayBuffer or pthreads: this is the single-threaded baseline.
add_link_options(-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576)

if(N2ENGINE_WEB_PROFILE STREQUAL "dev")
    # Fast build and fast link, debuggable: -O1 (no wasm-opt -O passes beyond emcc's minimal set), no LTO, DWARF debug
    # info (browsers' DevTools read it through the C/C++ DevTools extension; -gsource-map would add a .map file).
    add_compile_options(-O1 -g)
    add_link_options(-O1 -g)
elseif(N2ENGINE_WEB_PROFILE STREQUAL "size")
    # Smallest output: -Oz, LTO, wasm-opt -Oz (emcc runs wasm-opt at -Oz on its own), no debug info and no names
    # section (emcc strips both when -g is absent), assertions off. Exceptions and RTTI stay ON: the engine (sol2, the
    # standard library) is not known to build without them, so trimming them is a later, measured step.
    add_compile_options(-Oz -flto)
    add_link_options(-Oz -flto -sASSERTIONS=0)
else()
    # Fastest code: -O3 and LTO. Same link settings as size otherwise.
    add_compile_options(-O3 -flto)
    add_link_options(-O3 -flto -sASSERTIONS=0)
endif()

# Precompressed outputs (stage 3): index.<ext>.br and index.<ext>.gz next to the plain files of the web package. auto
# means on for the size and speed profiles, off for dev (the dev wasm is a few hundred MB of debug info).
set(N2ENGINE_WEB_PRECOMPRESS "auto" CACHE STRING "Write .br and .gz siblings of the web package files: auto (size and speed profiles), on or off")
set_property(CACHE N2ENGINE_WEB_PRECOMPRESS PROPERTY STRINGS auto on off)
if(N2ENGINE_WEB_PRECOMPRESS STREQUAL "auto")
    if(N2ENGINE_WEB_PROFILE STREQUAL "dev")
        set(N2ENGINE_WEB_PRECOMPRESS_EFFECTIVE OFF)
    else()
        set(N2ENGINE_WEB_PRECOMPRESS_EFFECTIVE ON)
    endif()
elseif(N2ENGINE_WEB_PRECOMPRESS STREQUAL "on")
    set(N2ENGINE_WEB_PRECOMPRESS_EFFECTIVE ON)
elseif(N2ENGINE_WEB_PRECOMPRESS STREQUAL "off")
    set(N2ENGINE_WEB_PRECOMPRESS_EFFECTIVE OFF)
else()
    message(FATAL_ERROR "N2ENGINE_WEB_PRECOMPRESS must be auto, on or off (got '${N2ENGINE_WEB_PRECOMPRESS}')")
endif()

# node runs the packaging scripts (scripts/web-package-finalize.mjs, scripts/web-budget-check.mjs): the one the
# Emscripten toolchain uses for the tests, or the one on PATH. Size budgets live in web/budgets.json only.
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(N2ENGINE_WEB_NODE ${CMAKE_CROSSCOMPILING_EMULATOR})
else()
    find_program(N2ENGINE_WEB_NODE_EXE node)
    if(NOT N2ENGINE_WEB_NODE_EXE)
        message(FATAL_ERROR "node is needed to package the web build (scripts/web-package-finalize.mjs)")
    endif()
    set(N2ENGINE_WEB_NODE ${N2ENGINE_WEB_NODE_EXE})
endif()

add_custom_target(web-size-report
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/web-size-report.sh" "${CMAKE_BINARY_DIR}/bin"
    COMMENT "Reporting the sizes of the built .wasm and .js files"
    VERBATIM
)

message(STATUS "Web: profile ${N2ENGINE_WEB_PROFILE} (N2ENGINE_WEB_PROFILE), single-threaded baseline")
