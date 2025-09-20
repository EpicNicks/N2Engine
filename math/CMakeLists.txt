# Collect all source files
file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Create the static library
add_library(math STATIC ${ENGINE_SOURCES})
add_library(MathCore::math ALIAS math)

# Set include directories
target_include_directories(math 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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

target_compile_options(math PUBLIC -msse2 -msse4.1)

# Add any necessary libraries
target_compile_definitions(math PRIVATE _USE_MATH_DEFINES)