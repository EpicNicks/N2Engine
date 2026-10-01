file(GLOB_RECURSE EDITOR_SERVER_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

add_executable(editor_server_tests ${EDITOR_SERVER_TEST_SOURCES})

target_link_libraries(editor_server_tests
        PRIVATE
        N2EditorServer
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(editor_server_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(editor_server_tests PRIVATE /W4)
else()
    target_compile_options(editor_server_tests PRIVATE -Wall -Wextra)
endif()

# ProtocolSpecTests checks the hand-written Protocol.hpp against the spec
target_compile_definitions(editor_server_tests PRIVATE
        N2_PROTOCOL_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../editor-server/protocol/protocol.json"
)

add_test(NAME editor_server_tests COMMAND editor_server_tests)
