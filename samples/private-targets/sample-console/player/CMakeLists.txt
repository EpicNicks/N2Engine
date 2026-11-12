# The sample private target's player (build targets stage 11; docs/private-targets.html). It lives outside the engine's
# folders and is built only when n2build passes this folder as N2ENGINE_EXTRA_TARGET_DIR: the engine's targets (engine,
# renderer, MathCore::math, and the desktop player's n2_player_options) are in scope, and no engine source names it.
#
# A real private target would add its SDK's libraries and toolchain here (the descriptor's cmake.toolchainFile).

add_executable(sample_console_player
    src/Main.cpp
    src/SampleConsoleHost.cpp
)
target_include_directories(sample_console_player PRIVATE src)
target_link_libraries(sample_console_player PRIVATE n2_player_options engine renderer MathCore::math)
set_target_properties(sample_console_player PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
if(WIN32)
    set_target_properties(sample_console_player PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set_target_properties(sample_console_player PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
endif()
n2_check_project_modules(sample_console_player)

# The files a package ships, for n2build (as players/desktop lists its own): the player, and OpenAL when it is shared
set(_sample_openal_shared "$<STREQUAL:$<TARGET_PROPERTY:OpenAL,TYPE>,SHARED_LIBRARY>")
if(WIN32)
    set(_sample_openal_file "$<TARGET_FILE:OpenAL>")
else()
    set(_sample_openal_file "$<IF:${_sample_openal_shared},$<TARGET_SONAME_FILE:OpenAL>,$<TARGET_FILE:OpenAL>>")
endif()
add_custom_command(TARGET sample_console_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sample_openal_file}" "$<TARGET_FILE_DIR:sample_console_player>/"
    COMMENT "Copying OpenAL beside the sample player"
)
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:sample_console_player>/sample-console-files.txt"
    CONTENT "$<TARGET_FILE_NAME:sample_console_player>$<${_sample_openal_shared}:\n$<PATH:GET_FILENAME,${_sample_openal_file}>>\n"
)
