# The web player's input translation (web/player/WebInputMap.*), its --renderer argument (RendererOptionTests.cpp,
# web/player/WebRendererOption.*) and the audio system's no-device path (WebAudioTests.cpp:
# node has no WebAudio) and what is specific to PhysX on WebAssembly (PhysXWebTests.cpp: the Logger error callback, the
# scalar single-threaded build), run under node like the other web tests
add_executable(web_tests InputMapTests.cpp RendererOptionTests.cpp WebAudioTests.cpp PhysXWebTests.cpp)

target_link_libraries(web_tests
        PRIVATE
        n2_web_input
        engine
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(web_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_compile_options(web_tests PRIVATE -Wall -Wextra)

add_test(NAME web_tests COMMAND web_tests)

add_executable(websocket_client WebSocketClient.cpp)
target_link_libraries(websocket_client PRIVATE n2net)
set_target_properties(websocket_client PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_link_options(websocket_client PRIVATE "-sEXIT_RUNTIME=1")
