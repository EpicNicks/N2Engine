# N2Server (netcode stage A6): the unit tests of its arguments and join-token generator, the test client of the smoke
# test, and the smoke test itself (tests/smoke/n2server_smoke.py runs the real N2Server end to end on loopback).

file(GLOB N2SERVER_TEST_SOURCES "N2Server*Tests.cpp")

add_executable(websocket_echo WebSocketEcho.cpp)
target_link_libraries(websocket_echo PRIVATE n2net)
set_target_properties(websocket_echo PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
find_program(N2_NODE_EXECUTABLE node)
if(N2_NODE_EXECUTABLE)
    execute_process(COMMAND ${N2_NODE_EXECUTABLE} -e "process.exit(typeof WebSocket === 'function' ? 0 : 1)"
        RESULT_VARIABLE N2_NODE_HAS_WEBSOCKET)
endif()
if(N2_NODE_EXECUTABLE AND N2_NODE_HAS_WEBSOCKET EQUAL 0)
    add_test(NAME websocket_smoke COMMAND ${N2_NODE_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/websocket-smoke.mjs $<TARGET_FILE:websocket_echo>)
    set_tests_properties(websocket_smoke PROPERTIES TIMEOUT 40)
endif()

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

# The world-protocol loopback test (netcode stage B1): a test host (N2Server's RunServer with hooks: it registers a prefab
# and runs spawn and despawn commands from stdin) and a headless test client (a NetworkClient over the secure handshake),
# started as real processes on loopback by tests/smoke/n2server_net.py. Both run the engine, so they need what N2Server
# needs (on Windows its PhysX DLLs, copied next to it, which the dependency on N2Server below guarantees).
add_executable(n2server_net_host NetHost.cpp)
target_link_libraries(n2server_net_host PRIVATE N2ServerCore)
add_executable(n2server_net_client NetClient.cpp)
target_link_libraries(n2server_net_client PRIVATE N2ServerCore)
foreach(NET_TOOL n2server_net_host n2server_net_client)
    set_target_properties(${NET_TOOL} PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
    )
    if (MSVC)
        target_compile_options(${NET_TOOL} PRIVATE /W4)
    else()
        target_compile_options(${NET_TOOL} PRIVATE -Wall -Wextra)
    endif()
    add_dependencies(${NET_TOOL} N2Server)
endforeach()

# The lockstep soak (netcode stage C3): a headless client that runs the C2 replay scene from the confirmed ticks of a
# `N2Server --relay`, started as real processes on loopback by tests/smoke/n2server_lockstep.py
add_executable(n2server_lockstep_client LockstepClient.cpp)
target_link_libraries(n2server_lockstep_client PRIVATE N2ServerCore)
set_target_properties(n2server_lockstep_client PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
if (MSVC)
    target_compile_options(n2server_lockstep_client PRIVATE /W4)
else()
    target_compile_options(n2server_lockstep_client PRIVATE -Wall -Wextra)
endif()
add_dependencies(n2server_lockstep_client N2Server)

# The smoke tests: Python 3 standard library only. Without an interpreter they are not registered (the unit tests above are).
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME n2server_smoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke/n2server_smoke.py
            --server $<TARGET_FILE:N2Server> --client $<TARGET_FILE:n2server_smoke_client>)
    set_tests_properties(n2server_smoke PROPERTIES TIMEOUT 600)

    add_test(NAME n2server_net
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke/n2server_net.py
            --server $<TARGET_FILE:N2Server> --host $<TARGET_FILE:n2server_net_host>
            --client $<TARGET_FILE:n2server_net_client>)
    set_tests_properties(n2server_net PROPERTIES TIMEOUT 600)

    # A build that has Jolt and the strict floating-point profile can start the Deterministic preset: for it, a refusal to
    # start is a failure, not a skip
    set(N2_LOCKSTEP_TEST_EXTRA_ARGS "")
    if (N2ENGINE_USE_JOLT AND N2ENGINE_STRICT_FP)
        set(N2_LOCKSTEP_TEST_EXTRA_ARGS --require-deterministic)
    endif()
    add_test(NAME n2server_lockstep
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke/n2server_lockstep.py
            --server $<TARGET_FILE:N2Server> --client $<TARGET_FILE:n2server_lockstep_client>
            --replay $<TARGET_FILE:n2_replay> ${N2_LOCKSTEP_TEST_EXTRA_ARGS})
    set_tests_properties(n2server_lockstep PROPERTIES TIMEOUT 600)
else()
    message(STATUS "N2Server smoke tests not registered: no Python 3 interpreter found")
endif()
