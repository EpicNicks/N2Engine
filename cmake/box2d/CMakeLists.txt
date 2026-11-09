# Box2D, vendored at external/box2d (see VENDORED.md there), as the static library target box2d (alias box2d::box2d). Added
# by the root CMakeLists.txt when N2ENGINE_USE_BOX2D is ON. Tests link it (tests/box2d).
#
# Our own target instead of upstream's src/CMakeLists.txt, so every option has one fixed value here: upstream turns wasm
# SIMD on for Emscripten and adds install, packaging and sample rules the engine does not need. Not set, so off: double
# precision (BOX2D_DOUBLE_PRECISION), heavy validation (BOX2D_VALIDATE), the profiler (BOX2D_PROFILE).

set(N2_BOX2D_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../external/box2d")
if(NOT EXISTS "${N2_BOX2D_ROOT}/include/box2d/box2d.h")
    message(FATAL_ERROR "N2ENGINE_USE_BOX2D=ON needs external/box2d (vendored Box2D): include/box2d/box2d.h is missing")
endif()

# The vendored tree is checked against SHA256SUMS (scripts/check-box2d-vendor.sh), and every .c file in it is upstream's
# source list, so a glob cannot pick up anything else
file(GLOB N2_BOX2D_SOURCES CONFIGURE_DEPENDS "${N2_BOX2D_ROOT}/src/*.c")

add_library(box2d STATIC ${N2_BOX2D_SOURCES})
add_library(box2d::box2d ALIAS box2d)

# SYSTEM: warnings from Box2D's headers must not appear in the engine's own code
set_target_properties(box2d PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    SYSTEM ON
)
target_include_directories(box2d
    PUBLIC "${N2_BOX2D_ROOT}/include"
    PRIVATE "${N2_BOX2D_ROOT}/src"
)

# == Floating point (PUBLIC: Box2D's headers hold inline math, compiled into every user) ==
# Fused multiply-add contraction is the one value-changing transform compilers enable by default, and Box2D's cross-platform
# determinism depends on it being off (https://box2d.org/posts/2024/08/determinism/)
if(MSVC)
    target_compile_options(box2d PUBLIC /fp:precise)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # clang-cl drops the plain driver flag, so it is passed through to the front end
        target_compile_options(box2d PUBLIC /clang:-ffp-contract=off)
    endif()
else()
    target_compile_options(box2d PUBLIC -ffp-contract=off)
endif()

# == Box2D's own translation units (PRIVATE) ==
if(MSVC)
    target_compile_options(box2d PRIVATE /w)
    # Box2D picks its solver width at runtime with cpuid (src/simd.c), so this file only runs on a CPU reporting AVX2. GCC
    # and Clang get the same through target attributes in src/core.h.
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|X86)$")
        set_source_files_properties("${N2_BOX2D_ROOT}/src/contact_solver_w8.c" PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    endif()
else()
    target_compile_options(box2d PRIVATE -w)
endif()

# The web player is built without wasm SIMD (Jolt too: USE_WASM_SIMD OFF in cmake/jolt), so Box2D must not add a browser
# requirement the player does not have. The results do not depend on it: tests/box2d's goldens hold with or without.
if(EMSCRIPTEN)
    target_compile_definitions(box2d PRIVATE BOX2D_DISABLE_SIMD)
endif()

# libm for sqrtf and friends, as upstream links it. No find_package(Threads) here, as upstream has none: it moved the first
# Threads check ahead of OpenAL Soft's configure probes, which flipped several of their results (HAVE_LIBATOMIC among them),
# so the Docker image's host needed a libatomic the image does not ship (PR #312). Only the internal scheduler uses threads,
# which needs workerCount above 1; glibc 2.34 and later has pthread in libc.
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(box2d PUBLIC m)
endif()

message(STATUS "Box2D (vendored, external/box2d): static library, single precision")
