# Runs the test scene from Lua: assets/scene.lua builds it, assets/scripts/*.lua drive it
add_executable(lua_project src/Main.cpp)

target_link_libraries(lua_project PRIVATE
    renderer
    engine
    MathCore::math
)

set_target_properties(lua_project PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Default project folder when none is passed on the command line
target_compile_definitions(lua_project PRIVATE
    N2_LUA_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(WIN32)
    set_target_properties(lua_project PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    foreach(DLL IN LISTS PHYSX_DLLS)
        add_custom_command(TARGET lua_project POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PHYSX_LIB_DIR}/${DLL}"
                "$<TARGET_FILE_DIR:lua_project>/${DLL}"
            COMMENT "Copying ${DLL}"
        )
    endforeach()
endif()
