# Collect all source files
file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Create the static library
add_library(engine STATIC ${ENGINE_SOURCES})

# Set include directories
target_include_directories(engine 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set target properties
set_target_properties(engine PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(engine PRIVATE N2ENGINE_DEBUG=1)
endif()

# Add any necessary libraries
# target_link_libraries(engine PUBLIC/PRIVATE other_libs)
target_link_libraries(engine PRIVATE renderer)
target_link_libraries(engine PRIVATE MathCore::math)