cmake_minimum_required(VERSION 3.20)

# N2Server: the headless dedicated game server (netcode stage A6). Native builds only: it needs the UDP transport and the
# crypto library, neither of which the web build has (the root CMakeLists.txt adds this folder outside the web build).
#
# N2ServerCore is the library (the argument parsing, the join-token generator and RunServer) so the tests can use them;
# N2Server is main() over it. It reuses the editor host's argument helpers, log sink and scene-path resolution from
# N2EditorServer (a static library: only what N2Server calls is linked into it).
file(GLOB N2SERVER_SOURCES "src/*.cpp")
# Main.cpp belongs to N2Server only; a main() in the library would hijack anything that links it
list(FILTER N2SERVER_SOURCES EXCLUDE REGEX "/Main\\.cpp$")

add_library(N2ServerCore STATIC ${N2SERVER_SOURCES})

target_include_directories(N2ServerCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(N2ServerCore
    PUBLIC
    N2EditorServer
    engine
    n2net # the transports and the handshake (not part of the engine target)
    n2security
    MathCore::math
)

set_target_properties(N2ServerCore PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(N2ServerCore PRIVATE /W4)
else()
    target_compile_options(N2ServerCore PRIVATE -Wall -Wextra)
endif()

add_executable(N2Server src/Main.cpp)
target_link_libraries(N2Server PRIVATE N2ServerCore)

set_target_properties(N2Server PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(WIN32)
    set_target_properties(N2ServerCore N2Server PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    # The PhysX DLLs next to the executable, as for lua_project
    foreach(DLL IN LISTS PHYSX_DLLS)
        add_custom_command(TARGET N2Server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PHYSX_LIB_DIR}/${DLL}"
                "$<TARGET_FILE_DIR:N2Server>/${DLL}"
            COMMENT "Copying ${DLL}"
        )
    endforeach()
endif()

install(TARGETS N2Server
    RUNTIME DESTINATION bin
)
