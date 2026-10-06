file(GLOB_RECURSE ASSETIMPORT_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

add_executable(assetimport_tests ${ASSETIMPORT_TEST_SOURCES})

# Only the assetimport library: these tests never link the engine or the renderer
target_link_libraries(assetimport_tests
        PRIVATE
        N2Engine::assetimport
        GTest::gtest
        GTest::gtest_main
)

# stb_image_write (test-only) encodes the JPEG, TGA and BMP test images; tests/common has the glTF file builder
target_include_directories(assetimport_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/external/stb_image/include
        ${CMAKE_SOURCE_DIR}/tests/common
)

set_target_properties(assetimport_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(assetimport_tests PRIVATE /W4)
else()
    target_compile_options(assetimport_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME assetimport_tests COMMAND assetimport_tests)
