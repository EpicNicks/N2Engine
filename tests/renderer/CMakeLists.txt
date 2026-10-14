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

set_target_properties(renderer_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(renderer_tests PRIVATE /W4)
else()
    target_compile_options(renderer_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME renderer_tests COMMAND renderer_tests)
