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
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
else()
    # GCC/Clang flags
    target_compile_options(math PUBLIC 
        -msse2 
        -msse4.1 
        -mavx
    )
endif()

# Add any necessary libraries
target_compile_definitions(math PRIVATE _USE_MATH_DEFINES)