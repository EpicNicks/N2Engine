file(GLOB_RECURSE ENGINE_TEST_SOURCES
        "*.cpp"
)

add_executable(engine_tests ${ENGINE_TEST_SOURCES})

target_link_libraries(engine_tests
        PRIVATE
        engine
        math
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(engine_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(engine_tests PRIVATE /W4)
else()
    target_compile_options(engine_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME engine_tests COMMAND engine_tests)
