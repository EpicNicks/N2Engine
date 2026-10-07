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

add_library(N2EditorServer STATIC
    ${EDITOR_SERVER_SOURCES}
)

target_include_directories(N2EditorServer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(N2EditorServer
    PUBLIC
    engine
    renderer
    MathCore::math
    PRIVATE
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

# The engine version Hello reports: the root project's (editor-server declares no project of its own)
target_compile_definitions(N2EditorServer PRIVATE N2ENGINE_VERSION="${PROJECT_VERSION}")

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