cmake_minimum_required(VERSION 3.20)

# Optional: generate protocol headers at build time
find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
    add_custom_command(
        OUTPUT
        ${CMAKE_CURRENT_SOURCE_DIR}/include/editor/protocol/Generated.hpp
        COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/protocol/generators/generate_cpp.py
        DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/protocol.json
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/generators/generate_cpp.py
        COMMENT "Generating C++ protocol bindings"
    )

    add_custom_target(N2ProtocolGenerate
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/editor/protocol/Generated.hpp
    )
endif()

file(GLOB_RECURSE EDITOR_SERVER_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

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

if(TARGET N2ProtocolGenerate)
    add_dependencies(N2EditorServer N2ProtocolGenerate)
endif()

# Standalone editor host
add_executable(N2EditorHost src/main.cpp)
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