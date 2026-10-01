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

# With the FreeType backend built, a few tests also call FreeType directly (its version and modules)
if (N2ENGINE_TEXT_FREETYPE)
    target_link_libraries(text_tests PRIVATE freetype)
    target_compile_definitions(text_tests PRIVATE N2ENGINE_TEXT_FREETYPE=1)
endif()

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
