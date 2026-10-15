if(N2ENGINE_PLATFORM_WEB)
    # The tests run under node: exit the runtime when main returns so ctest sees the exit code and the output is flushed
    add_link_options(-sEXIT_RUNTIME=1)
    # ... and read and write the host's files directly (node's fs, no virtual filesystem): the engine tests open the
    # sources' own assets (lua_project/, tests/assets/, renderer/shaders/) by absolute path and write temporary files
    # under the system temp directory. Tests only: a shipped web build uses Emscripten's virtual filesystem.
    add_link_options(-sNODERAWFS=1)
endif()
add_subdirectory(math)
add_subdirectory(text)
add_subdirectory(assetimport)
add_subdirectory(engine)
add_subdirectory(renderer)
if(NOT N2ENGINE_PLATFORM_WEB)
# The editor host (raw sockets, TLS) is not part of a player
add_subdirectory(editor-server)
endif()
