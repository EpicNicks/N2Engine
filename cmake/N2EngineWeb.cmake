# Web (Emscripten) platform support. Included from the root CMakeLists.txt only when N2ENGINE_PLATFORM_WEB is ON, which
# is derived from the toolchain (an Emscripten.cmake toolchain file, i.e. `emcmake cmake`), never set by hand. Native
# configurations never include this file.
#
# What this file does:
#   * defines the N2ENGINE_PLATFORM_WEB compile definition for every target
#   * defines the N2ENGINE_WEB_PROFILE cache variable (dev | size | speed) and turns it into compile and link flags
#   * defines the optional size budget variables and a `web-size-report` target that runs scripts/web-size-report.sh
#
# The flags are global (add_compile_options / add_link_options) so that every library and executable, including the
# fetched googletest, is built the same way; LTO in particular needs every object to be compiled with it.

set(N2ENGINE_WEB_PROFILE "dev" CACHE STRING "Web build profile: dev (fast build), size (smallest output) or speed (fastest code)")
set_property(CACHE N2ENGINE_WEB_PROFILE PROPERTY STRINGS dev size speed)

if(NOT N2ENGINE_WEB_PROFILE MATCHES "^(dev|size|speed)$")
    message(FATAL_ERROR "N2ENGINE_WEB_PROFILE must be dev, size or speed (got '${N2ENGINE_WEB_PROFILE}')")
endif()

add_compile_definitions(N2ENGINE_PLATFORM_WEB=1)

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

# Optional size budgets, in bytes. Empty (the default) means no budget. They are read by scripts/web-size-report.sh,
# which fails only for a budget that is set and exceeded.
set(N2ENGINE_WEB_BUDGET_WASM_GZIP "" CACHE STRING "Fail web-size-report when the gzip size of any one .wasm exceeds this many bytes (empty: no budget)")
set(N2ENGINE_WEB_BUDGET_TOTAL_GZIP "" CACHE STRING "Fail web-size-report when the gzip size of all .wasm and .js files together exceeds this many bytes (empty: no budget)")

set(N2ENGINE_WEB_SIZE_ENV "")
if(NOT N2ENGINE_WEB_BUDGET_WASM_GZIP STREQUAL "")
    list(APPEND N2ENGINE_WEB_SIZE_ENV "N2ENGINE_WEB_BUDGET_WASM_GZIP=${N2ENGINE_WEB_BUDGET_WASM_GZIP}")
endif()
if(NOT N2ENGINE_WEB_BUDGET_TOTAL_GZIP STREQUAL "")
    list(APPEND N2ENGINE_WEB_SIZE_ENV "N2ENGINE_WEB_BUDGET_TOTAL_GZIP=${N2ENGINE_WEB_BUDGET_TOTAL_GZIP}")
endif()

add_custom_target(web-size-report
    COMMAND ${CMAKE_COMMAND} -E env ${N2ENGINE_WEB_SIZE_ENV}
            bash "${CMAKE_SOURCE_DIR}/scripts/web-size-report.sh" "${CMAKE_BINARY_DIR}/bin"
    COMMENT "Reporting the sizes of the built .wasm and .js files"
    VERBATIM
)

message(STATUS "Web: profile ${N2ENGINE_WEB_PROFILE} (N2ENGINE_WEB_PROFILE), single-threaded baseline")
