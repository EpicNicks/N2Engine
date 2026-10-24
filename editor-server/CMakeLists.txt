cmake_minimum_required(VERSION 3.20)

# Optional: generate protocol headers at build time
find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
    # The output is a stamp in the build tree: the generated header is checked in, so naming it as the
    # OUTPUT would let a clean delete it, and an OUTPUT the command never writes reran it every build.
    # The script only rewrites the header when its content changes.
    set(N2_PROTOCOL_STAMP ${CMAKE_CURRENT_BINARY_DIR}/N2ProtocolGenerate.stamp)
    add_custom_command(
        OUTPUT
        ${N2_PROTOCOL_STAMP}
        COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/protocol/generators/generate_cpp.py
        COMMAND
        ${CMAKE_COMMAND} -E touch ${N2_PROTOCOL_STAMP}
        DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/protocol.json
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/generators/generate_cpp.py
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/generators/protocol_spec.py
        COMMENT "Generating C++ protocol bindings"
    )

    add_custom_target(N2ProtocolGenerate
        DEPENDS ${N2_PROTOCOL_STAMP}
    )
endif()

file(GLOB_RECURSE EDITOR_SERVER_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)
# Main.cpp belongs to N2EditorHost only; a main() in the library would hijack anything that links it
list(FILTER EDITOR_SERVER_SOURCES EXCLUDE REGEX "/Main\.cpp$")

# TLS for the editor port (--tls): Mbed TLS, vendored unmodified under external/mbedtls, is optional (like the text
# library's FreeType and the asset importer's ufbx). The option is declared in the root CMakeLists.txt. OFF leaves X.509
# and TLS out entirely: the host then refuses --tls and behaves exactly as before. The library is built once for the
# whole tree by cmake/mbedtls/: n2_mbedcrypto (crypto only, in every native build: n2security uses it) and, with the
# option, n2_mbedtls_tls (X.509 and TLS) on top of it, both with one configuration (threading on, std::mutex callbacks
# that n2security installs). EditorTls.cpp is the only file here that includes its headers, and no mbedtls type reaches
# a public header.
add_library(N2EditorServer STATIC
    ${EDITOR_SERVER_SOURCES}
)

if(N2ENGINE_EDITOR_TLS)
    # n2_mbedtls_tls brings the include directories and MBEDTLS_USER_CONFIG_FILE (PUBLIC there), and BCrypt on Windows
    target_link_libraries(N2EditorServer PRIVATE n2_mbedtls_tls)
    target_compile_definitions(N2EditorServer PRIVATE N2ENGINE_EDITOR_TLS=1)
    message(STATUS "Editor host: TLS enabled (--tls; Mbed TLS, vendored)")
endif()

target_include_directories(N2EditorServer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(N2EditorServer
    PUBLIC
    engine
    renderer
    MathCore::math
    # PeerAddress, RateLimiter, TokenBucket, PreAuthSlots and the token files (shared with the netcode); its crypto
    # (InitializeCrypto) is what EditorTls.cpp calls for the library's one-time setup
    n2security
    PRIVATE
    assetimport
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

if(TARGET N2ProtocolGenerate)
    add_dependencies(N2EditorServer N2ProtocolGenerate)
endif()

# Standalone editor host
add_executable(N2EditorHost src/Main.cpp)
target_link_libraries(N2EditorHost PRIVATE N2EditorServer)

if (WIN32)
    set_target_properties(N2EditorServer N2EditorHost PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# Install
install(TARGETS N2EditorServer N2EditorHost
    EXPORT N2EngineTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)

# Install protocol.json for client generators
install(FILES protocol/protocol.json DESTINATION share/n2engine/protocol)
install(DIRECTORY protocol/generators DESTINATION share/n2engine/protocol)
