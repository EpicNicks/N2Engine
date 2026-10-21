# N2Server (netcode stage A6): the unit tests of its arguments and join-token generator, the test client of the smoke
# test, and the smoke test itself (tests/smoke/n2server_smoke.py runs the real N2Server end to end on loopback).

file(GLOB N2SERVER_TEST_SOURCES "N2Server*Tests.cpp")

add_executable(n2server_tests ${N2SERVER_TEST_SOURCES})
target_link_libraries(n2server_tests
        PRIVATE
        N2ServerCore
        GTest::gtest
        GTest::gtest_main
)
set_target_properties(n2server_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
if (MSVC)
    target_compile_options(n2server_tests PRIVATE /W4)
else()
    target_compile_options(n2server_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME n2server_tests COMMAND n2server_tests)

# The test client: joins a running N2Server through the A5 handshake (it links the library for the protocol id parser)
add_executable(n2server_smoke_client SmokeClient.cpp)
target_link_libraries(n2server_smoke_client PRIVATE N2ServerCore)
set_target_properties(n2server_smoke_client PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
if (MSVC)
    target_compile_options(n2server_smoke_client PRIVATE /W4)
else()
    target_compile_options(n2server_smoke_client PRIVATE -Wall -Wextra)
endif()
# Building the client builds the server the smoke test starts
add_dependencies(n2server_smoke_client N2Server)

# The smoke test: Python 3 standard library only. Without an interpreter it is not registered (the unit tests above are).
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME n2server_smoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke/n2server_smoke.py
            --server $<TARGET_FILE:N2Server> --client $<TARGET_FILE:n2server_smoke_client>)
    set_tests_properties(n2server_smoke PROPERTIES TIMEOUT 600)
else()
    message(STATUS "N2Server smoke test not registered: no Python 3 interpreter found")
endif()
