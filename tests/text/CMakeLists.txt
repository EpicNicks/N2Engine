file(GLOB_RECURSE TEXT_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

add_executable(text_tests ${TEXT_TEST_SOURCES})

# Only the text library: these tests never link the engine or the renderer
target_link_libraries(text_tests
        PRIVATE
        N2Engine::text
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(text_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(text_tests PRIVATE /W4)
else()
    target_compile_options(text_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME text_tests COMMAND text_tests)
