if(N2ENGINE_PLATFORM_WEB)
    # The tests run under node: exit the runtime when main returns so ctest sees the exit code and the output is flushed
    add_link_options(-sEXIT_RUNTIME=1)
    # ... and read and write the host's files directly (node's fs, no virtual filesystem): the engine tests open the
    # sources' own assets (lua_project/, tests/assets/, renderer/shaders/) by absolute path and write temporary files
    # under the system temp directory. Tests only: a shipped web build uses Emscripten's virtual filesystem.
    add_link_options(-sNODERAWFS=1)
    # The program's environment variables (HOME, TMPDIR) are node's, as in a native run: Emscripten's default is a
    # fixed HOME of /home/web_user, which is not a folder on the host
    add_link_options(-sEXPORTED_RUNTIME_METHODS=ENV "--pre-js=${CMAKE_SOURCE_DIR}/tests/web/node-env.js")
endif()
add_subdirectory(math)
add_subdirectory(text)
add_subdirectory(assetimport)
add_subdirectory(engine)
add_subdirectory(renderer)
if(N2ENGINE_USE_JOLT)
# The vendored Jolt Physics: a smoke test of the library alone (no engine target is linked)
add_subdirectory(jolt)
endif()
if(N2ENGINE_PLATFORM_WEB)
# The web player's input translation (the rest of the player needs a browser or the package smoke test)
add_subdirectory(web)
endif()
if(NOT N2ENGINE_PLATFORM_WEB)
# The editor host (raw sockets, TLS) is not part of a player
add_subdirectory(editor-server)
# N2Server's tests and smoke test (UDP sockets and the crypto library: not in the web build either)
add_subdirectory(n2server)
endif()
