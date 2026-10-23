# Jolt Physics, vendored at external/JoltPhysics (see VENDORED.md there), as the static library target Jolt (alias
# Jolt::Jolt). Added by the root CMakeLists.txt when N2ENGINE_USE_JOLT is ON. The engine links it (engine/CMakeLists.txt:
# the Jolt physics backend, engine/src/physics/jolt/JoltBackend.cpp) and so does the smoke test (tests/jolt).
#
# Jolt's own Jolt/Jolt.cmake builds the target; the options it reads are set here, always to the same values, because
# the library is built ONE way: cross-platform deterministic (JPH_CROSS_PLATFORM_DETERMINISTIC), single precision,
# no debug renderer, no profiler, no compute, no object stream. Upstream's Build/CMakeLists.txt (not vendored) also
# turns the compiler's floating-point and language flags into global flags; the same flags are set below on the target
# (PUBLIC where the headers' inline math is compiled into every user of the library, PRIVATE where only Jolt's own
# translation units are concerned). A user of Jolt::Jolt therefore gets the instruction set and floating-point
# flags without setting them.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/JoltPhysics/Jolt/Jolt.cmake")
    message(FATAL_ERROR "N2ENGINE_USE_JOLT=ON needs external/JoltPhysics (vendored Jolt Physics): Jolt/Jolt.cmake is missing")
endif()

set(PHYSICS_REPO_ROOT "${CMAKE_SOURCE_DIR}/external/JoltPhysics")

# == Options read by Jolt/Jolt.cmake (every one has a fixed value here) ==
# The point of the library: the same simulation on every platform. Never an option of ours.
set(CROSS_PLATFORM_DETERMINISTIC ON)
set(DOUBLE_PRECISION OFF)
set(OBJECT_LAYER_BITS 16)
set(JPH_BUILD_SHARED_LIBS OFF)
set(GENERATE_DEBUG_SYMBOLS OFF)
set(CROSS_COMPILE_ARM OFF)
# Jolt turns its asserts on by itself whenever NDEBUG is not defined (Core/IssueReporting.h), so Debug has them and the
# release configurations do not; this option would add them to the release configurations too
set(USE_ASSERTS OFF)
# One link-time optimisation policy for the whole engine (the web profiles set theirs globally), not Jolt's own
set(INTERPROCEDURAL_OPTIMIZATION OFF)
# MSVC only, and only with Debug and Release: it would trap on a floating-point exception inside the simulation
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(DISABLE_CUSTOM_ALLOCATOR OFF)
set(USE_STD_VECTOR OFF)
set(TRACK_BROADPHASE_STATS OFF)
set(TRACK_NARROWPHASE_STATS OFF)
set(JPH_TRACK_SIMULATION_STATS OFF)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(DEBUG_RENDERER_IN_DISTRIBUTION OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DISTRIBUTION OFF)
set(JPH_USE_EXTERNAL_PROFILE OFF)
# Not needed (no serialisation of shapes or scenes through Jolt's ObjectStream)
set(ENABLE_OBJECT_STREAM OFF)
# Compute (hair simulation on a GPU or a CPU fallback): not used, so none of the DirectX 12, Vulkan, Metal or CPU
# compute sources and none of their SDK dependencies. Jolt falls back to a CreateComputeSystem() that reports an error.
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK OFF)
set(JPH_USE_MTL OFF)
set(JPH_USE_CPU_COMPUTE OFF)
# x86: the engine's floor is AVX (math/CMakeLists.txt), so no AVX2 or AVX-512 and none of the instructions that
# arrived with them (LZCNT, TZCNT and F16C are Haswell). Jolt's cross-platform claim is that its results do not depend
# on the instruction set, so this is a choice about which CPUs run it, not about what it computes.
set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_AVX ON)
set(USE_AVX2 OFF)
set(USE_AVX512 OFF)
set(USE_LZCNT OFF)
set(USE_TZCNT OFF)
set(USE_F16C OFF)
# Fused multiply-add changes results, so never (Jolt.cmake also drops it under CROSS_PLATFORM_DETERMINISTIC)
set(USE_FMADD OFF)
# Web: scalar, as the rest of the web build (docs/web-build.html), and 32-bit wasm
set(USE_WASM_SIMD OFF)
set(JPH_USE_WASM64 OFF)

if(MSVC)
    # Upstream turns C++ exceptions off for Jolt (it throws none); /EHsc is a default flag, so it is removed for this
    # directory only (C4577: the STL says noexcept without exception handling). RTTI is turned off per target below.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4577")
endif()

# Defines the target Jolt, the alias Jolt::Jolt and (through Jolt's own options above) the PUBLIC definitions
# JPH_CROSS_PLATFORM_DETERMINISTIC, JPH_OBJECT_LAYER_BITS=16, NDEBUG for the release configurations, JPH_USE_AVX, JPH_USE_SSE4_1
# and JPH_USE_SSE4_2 on x86 and, on the web, no pthread flags. It sets CMAKE_CXX_STANDARD 17 for this directory (the
# engine's C++23 is not touched: the setting is local to this directory).
include("${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake")

# Two settings of the engine tree do not suit Jolt:
#  - Jolt's precompiled header: every .cpp already includes <Jolt/Jolt.h> first, and a precompiled header would make
#    the ccache of the CI legs unable to cache them (MSVC, and Clang and GCC without extra ccache settings)
#  - the headers as ordinary includes: warnings from Jolt's headers must not appear in the engine's own code
set_target_properties(Jolt PROPERTIES
    DISABLE_PRECOMPILE_HEADERS ON
    SYSTEM ON
)

# == Floating point (PUBLIC: Jolt's headers hold inline vector and matrix math, compiled into every user) ==
# No fast-math anywhere (nothing here sets it, and the engine's strict-FP profile forbids it); fused multiply-add
# contraction off; x87 is never used on x86-64 (-mfpmath=sse comes from Jolt.cmake, MSVC x64 is SSE by definition).
if(MSVC)
    # cl and clang-cl: /fp:precise stated explicitly, as in the root CMakeLists.txt (never /fp:fast or /fp:contract)
    target_compile_options(Jolt PUBLIC /fp:precise)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(Jolt PUBLIC -ffp-contract=off)
    endif()
    # Jolt.cmake picks /arch:AVX only for Visual Studio generators (it tests CMAKE_VS_PLATFORM_NAME, empty under Ninja)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64|amd64)$")
        target_compile_options(Jolt PUBLIC /arch:AVX)
    endif()
else()
    # GCC contracts by default (-ffp-contract=fast) and Clang does within a statement: both fuse a * b + c as soon as
    # the target has an FMA instruction (aarch64 always does; x86 only with -mfma, which is not enabled)
    target_compile_options(Jolt PUBLIC -ffp-contract=off)
endif()

# == Jolt's own translation units (PRIVATE) ==
# No RTTI and no exceptions, as upstream builds it (Jolt has its own runtime type information and throws nothing), except
# exceptions on the web. The engine's code keeps its own settings: a library compiled without them links into a program
# with them.
if(MSVC)
    target_compile_options(Jolt PRIVATE /GR- /w)
elseif(EMSCRIPTEN)
    # Not -fno-exceptions: the web build compiles everything with -fwasm-exceptions (cmake/N2EngineWeb.cmake), and
    # Emscripten rejects that flag together with -fno-exceptions (it then wants setjmp/longjmp support instead)
    target_compile_options(Jolt PRIVATE -fno-rtti -w)
else()
    target_compile_options(Jolt PRIVATE -fno-rtti -fno-exceptions -w)
endif()

message(STATUS "Jolt Physics (vendored, external/JoltPhysics): static library, cross-platform deterministic, single precision")

# == Web: what Jolt costs in a wasm module (docs/web-build.html, "Jolt Physics: measured size") ==
# Two tiny programs, neither part of the player package: a baseline that does nothing and a probe that runs the same
# scenario as the smoke test (tests/jolt/JoltScenario.hpp: registers every type, builds a physics system, steps it and
# prints the state hash). The difference between their .wasm files is what the linked, used part of Jolt weighs, in the
# profile being built (scripts/web-jolt-size-report.mjs prints it). The probe also returns a failure when its hash is
# not the golden one, so running it under node is one more cross-build determinism check (size and speed profiles).
if(N2ENGINE_PLATFORM_WEB)
    add_executable(jolt_size_baseline EXCLUDE_FROM_ALL probe/JoltSizeBaseline.cpp)
    add_executable(jolt_size_probe EXCLUDE_FROM_ALL probe/JoltSizeProbe.cpp)
    target_link_libraries(jolt_size_probe PRIVATE Jolt::Jolt)
    target_include_directories(jolt_size_probe PRIVATE "${CMAKE_SOURCE_DIR}/tests/jolt")
    foreach(_n2_probe jolt_size_baseline jolt_size_probe)
        set_target_properties(${_n2_probe} PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        # Exit the runtime when main returns so node sees main's return value and the output is flushed
        target_link_options(${_n2_probe} PRIVATE -sEXIT_RUNTIME=1)
    endforeach()
endif()
