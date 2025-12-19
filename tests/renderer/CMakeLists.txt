file(GLOB_RECURSE RENDERER_TEST_SOURCES
        "*.cpp"
)

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
