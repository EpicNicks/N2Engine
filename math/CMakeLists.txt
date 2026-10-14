# Collect all source files
file(GLOB_RECURSE MATH_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Create the static library
add_library(math STATIC ${MATH_SOURCES})
add_library(MathCore::math ALIAS math)

# Set include directories
target_include_directories(math 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/nlohmann/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/TinySHA1/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
    src
)

# Set target properties
set_target_properties(math PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    # MSVC flags for SSE/AVX
    target_compile_options(math PUBLIC 
        /arch:AVX         # Enables AVX (includes SSE2, SSE4.1)
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|AMD64|amd64|x86|i[3-6]86)$")
    # GCC/Clang flags (x86 only; other architectures such as aarch64 build the scalar implementations)
    target_compile_options(math PUBLIC 
        -msse2 
        -msse4.1 
        -mavx
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|AARCH64|arm64|ARM64)$")
    # aarch64 (NEON is baseline, so no architecture flag): turn off fused multiply-add contraction. Clang contracts
    # a * b + c in scalar code into one rounding by default on this target, which x86 (no -mfma) never does, so the
    # scalar implementations would differ in the last bit from the NEON ones (which multiply and add separately) and
    # from the x86 results. PUBLIC for the same reason as the x86 flags: the inline scalar functions in the headers
    # are compiled into every target that links math, and must all be the same code
    target_compile_options(math PUBLIC
        -ffp-contract=off
    )
endif()

# Add any necessary libraries
target_compile_definitions(math PRIVATE _USE_MATH_DEFINES)