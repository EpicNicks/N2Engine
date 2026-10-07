file(GLOB_RECURSE EDITOR_SERVER_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)
# Compiled on its own below: it can't share a program with the server's headers
list(FILTER EDITOR_SERVER_TEST_SOURCES EXCLUDE REGEX "/GeneratedClientCheck\\.cpp$")

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

# ProtocolSpecTests checks the hand-written Protocol.hpp against the spec; ProtocolVectorTests checks the server's
# codecs against the golden vectors generated from it
target_compile_definitions(editor_server_tests PRIVATE
        N2_PROTOCOL_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../editor-server/protocol/protocol.json"
        N2_PROTOCOL_TEST_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../../editor-server/protocol/test-vectors.json"
)

add_test(NAME editor_server_tests COMMAND editor_server_tests)

# The generated C++ client compiles (an object library nothing links: it is never part of a program)
add_library(editor_protocol_generated_check OBJECT GeneratedClientCheck.cpp)
target_include_directories(editor_protocol_generated_check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../editor-server/clients/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../editor-server/include
)
set_target_properties(editor_protocol_generated_check PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
if (WIN32)
    set_target_properties(editor_protocol_generated_check PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
add_dependencies(editor_server_tests editor_protocol_generated_check)
