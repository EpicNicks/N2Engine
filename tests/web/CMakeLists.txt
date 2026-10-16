# The web player's input translation (web/player/WebInputMap.*), run under node like the other web tests
add_executable(web_tests InputMapTests.cpp)

target_link_libraries(web_tests
        PRIVATE
        n2_web_input
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
