# The desktop player (build targets stage 1; docs/build-targets.html#player): runs one game folder, and is what
# tools/n2build packages for the windows and linux targets. Native only (the web has web/).
#
#   n2_player_options  the command line, parsed without the engine running (unit tested: tests/players)
#   n2player           the player
#
# Beside the executable, n2player-files.txt lists every file a package must ship with it: the executable and the shared
# libraries it loads (OpenAL when it is built shared, PhysX's DLLs on Windows). n2build copies exactly that list, so a
# new runtime file is added here, not in the tool.

add_library(n2_player_options STATIC src/PlayerOptions.cpp)
target_include_directories(n2_player_options PUBLIC src)
target_link_libraries(n2_player_options PUBLIC engine)
set_target_properties(n2_player_options PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(n2player src/Main.cpp)
target_link_libraries(n2player PRIVATE n2_player_options renderer engine MathCore::math)
set_target_properties(n2player PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# A game built with its module list checks the list against the game when the player links (docs/project-modules.html)
n2_check_project_modules(n2player)

if(WIN32)
    set_target_properties(n2_player_options n2player PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # A Release player is a windowed program (no console window behind the game); --console-log still writes to a
    # console when started from one. Debug keeps the console, where the log goes.
    if(MSVC)
        target_link_options(n2player PRIVATE $<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
    endif()
    foreach(DLL IN LISTS PHYSX_DLLS)
        add_custom_command(TARGET n2player POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PHYSX_LIB_DIR}/${DLL}"
                "$<TARGET_FILE_DIR:n2player>/${DLL}"
            COMMENT "Copying ${DLL}"
        )
    endforeach()
else()
    # Shared libraries are looked for beside the executable, so a package folder runs wherever it is unpacked
    # (@executable_path is macOS's spelling of $ORIGIN)
    if(APPLE)
        set_target_properties(n2player PROPERTIES BUILD_RPATH "@executable_path" INSTALL_RPATH "@executable_path")
    else()
        set_target_properties(n2player PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
    endif()
endif()

# OpenAL Soft (fetched by the root CMakeLists.txt after this folder, so only generator expressions name it): when it is a
# shared library, a copy beside the player, under its SONAME on Linux
set(_n2_openal_shared "$<STREQUAL:$<TARGET_PROPERTY:OpenAL,TYPE>,SHARED_LIBRARY>")
if(WIN32)
    set(_n2_openal_file "$<${_n2_openal_shared}:$<TARGET_FILE:OpenAL>>")
else()
    set(_n2_openal_file "$<${_n2_openal_shared}:$<TARGET_SONAME_FILE:OpenAL>>")
endif()
# Copied whatever its type (a static library copied beside the player is harmless, and only a shared one is listed):
# a command that expands to nothing is not portable across generators
if(WIN32)
    set(_n2_openal_copy "$<TARGET_FILE:OpenAL>")
else()
    set(_n2_openal_copy "$<IF:${_n2_openal_shared},$<TARGET_SONAME_FILE:OpenAL>,$<TARGET_FILE:OpenAL>>")
endif()
add_custom_command(TARGET n2player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_n2_openal_copy}" "$<TARGET_FILE_DIR:n2player>/"
    COMMENT "Copying OpenAL beside the player"
)

set(_n2_player_files "$<TARGET_FILE_NAME:n2player>")
foreach(DLL IN LISTS PHYSX_DLLS)
    if(WIN32)
        string(APPEND _n2_player_files "\n${DLL}")
    endif()
endforeach()
string(APPEND _n2_player_files "$<${_n2_openal_shared}:\n$<PATH:GET_FILENAME,${_n2_openal_file}>>")

# macOS with the Vulkan renderer on MoltenVK (cmake/N2EngineApple.cmake; build targets stage 13): MoltenVK is copied beside
# the player and the player loads it from there (its install name rewritten to @executable_path), so a package runs on a
# Mac with nothing installed. Rewriting a binary breaks its signature, which Apple silicon refuses to run: both are signed
# again (ad hoc; a release is signed by its own identity later)
if(APPLE AND N2ENGINE_APPLE_MOLTENVK_LINKED)
    add_custom_command(TARGET n2player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${N2_MOLTENVK_LIBRARY}" "$<TARGET_FILE_DIR:n2player>/libMoltenVK.dylib"
        COMMAND chmod u+w "$<TARGET_FILE_DIR:n2player>/libMoltenVK.dylib"
        COMMAND install_name_tool -id @executable_path/libMoltenVK.dylib "$<TARGET_FILE_DIR:n2player>/libMoltenVK.dylib"
        COMMAND sh -c "install_name_tool -change \"$(otool -D '${N2_MOLTENVK_LIBRARY}' | tail -1)\" @executable_path/libMoltenVK.dylib '$<TARGET_FILE:n2player>'"
        COMMAND codesign --force --sign - "$<TARGET_FILE_DIR:n2player>/libMoltenVK.dylib"
        COMMAND codesign --force --sign - "$<TARGET_FILE:n2player>"
        COMMENT "MoltenVK beside the player"
        VERBATIM
    )
    string(APPEND _n2_player_files "\nlibMoltenVK.dylib")
endif()
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:n2player>/n2player-files.txt"
    CONTENT "${_n2_player_files}\n"
)
