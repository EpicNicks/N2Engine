# The desktop player's command line (players/desktop/src/PlayerOptions.*). The player itself needs a window: its smoke
# run is tools/n2build's (docs/build-targets.html)
add_executable(player_tests PlayerOptionsTests.cpp)

target_link_libraries(player_tests
        PRIVATE
        n2_player_options
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(player_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if(WIN32)
    set_target_properties(player_tests PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

add_test(NAME player_tests COMMAND player_tests)
