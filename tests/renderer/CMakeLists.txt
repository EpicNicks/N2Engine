file(GLOB_RECURSE RENDERER_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

# The Vulkan tests need the Vulkan backend (N2ENGINE_RENDERER_VULKAN)
if(NOT N2ENGINE_RENDERER_VULKAN)
    list(FILTER RENDERER_TEST_SOURCES EXCLUDE REGEX "/vulkan/")
endif()

add_executable(renderer_tests ${RENDERER_TEST_SOURCES})

target_link_libraries(renderer_tests
        PRIVATE
        renderer
        engine
        math
        GTest::gtest
        GTest::gtest_main
)

# The embedded WGSL (the generated header of renderer/CMakeLists.txt: kUnlitWgsl, kLitWgsl, kTextWgsl), which the WebGPU shader
# parity test compares with the files
target_link_libraries(renderer_tests PRIVATE renderer_webgpu_shaders)

set_target_properties(renderer_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

# The Vulkan shader parity test reads the GLSL under renderer/shaders/vulkan/ as text (no device needed)
target_compile_definitions(renderer_tests PRIVATE
        N2_RENDERER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/renderer"
)

if (MSVC)
    target_compile_options(renderer_tests PRIVATE /W4)
else()
    target_compile_options(renderer_tests PRIVATE -Wall -Wextra)
endif()

# tests/common has the glTF file builder the Vulkan model golden (tests/renderer/vulkan/VulkanModelGoldenTests.cpp) shares
# with tests/engine and tests/assetimport
target_include_directories(renderer_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)

# tests/renderer/common has the cross-backend test helpers (CrossBackendDiff.hpp, BlendReference.hpp) every GPU backend's
# tests share: included as "common/<name>.hpp" from tests/renderer
target_include_directories(renderer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME renderer_tests COMMAND renderer_tests)
