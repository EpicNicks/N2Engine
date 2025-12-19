file(GLOB_RECURSE MATH_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

add_executable(math_tests ${MATH_TEST_SOURCES})

target_link_libraries(math_tests
        PRIVATE
        math
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(math_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

# Optional: stronger warnings for tests
if (MSVC)
    target_compile_options(math_tests PRIVATE /W4)
else()
    target_compile_options(math_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME math_tests COMMAND math_tests)
