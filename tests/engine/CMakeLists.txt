file(GLOB_RECURSE ENGINE_TEST_SOURCES
        "*.cpp"
        "*.cc"
        "*.cxx"
)

# Web (Emscripten, node): whole test files that cannot run in the single-threaded headless build, each with its reason.
# Nothing here applies to a native build, and a path that no longer exists fails the configure, so the list cannot go
# stale. Tests that need only a few lines skipped inside a file that otherwise runs use GTEST_SKIP under
# N2ENGINE_PLATFORM_WEB instead (they show as skipped in the output: SerializationRobustnessTests.cpp's audio fixture,
# CoreRobustnessTests.cpp's multi-threaded logger test). docs/web-build.html lists both.
if(N2ENGINE_PLATFORM_WEB)
    set(N2ENGINE_WEB_EXCLUDED_ENGINE_TESTS
        # OpenAL: the web build has no audio device yet (external/web-stubs), and these open OpenAL Soft's null and
        # loopback devices
        audio/AudioTests.cpp
        audio/LoopbackAudioTests.cpp
    )
    foreach(_excluded IN LISTS N2ENGINE_WEB_EXCLUDED_ENGINE_TESTS)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_excluded}")
            message(FATAL_ERROR "N2ENGINE_WEB_EXCLUDED_ENGINE_TESTS names a file that does not exist: ${_excluded}")
        endif()
        list(REMOVE_ITEM ENGINE_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_excluded}")
        message(STATUS "Web: engine_tests excludes ${_excluded}")
    endforeach()
endif()

add_executable(engine_tests ${ENGINE_TEST_SOURCES})

target_link_libraries(engine_tests
        PRIVATE
        engine
        renderer # engine's scene headers include renderer headers
        math
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(engine_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

if (MSVC)
    target_compile_options(engine_tests PRIVATE /W4)
else()
    target_compile_options(engine_tests PRIVATE -Wall -Wextra)
endif()

# lua_project/assets/scene.lua is built headless by LuaSceneTests; tests/assets holds the committed test models
target_compile_definitions(engine_tests PRIVATE
        N2_LUA_PROJECT_DIR="${CMAKE_SOURCE_DIR}/lua_project"
        N2_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/tests/assets"
        N2_RENDERER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/renderer" # OpenGlShaderSourceTests reads the GLSL as text
)

# With the optional FBX/OBJ importer (N2ENGINE_MODEL_UFBX) the model tests also load the committed FBX; without it they
# check that an .fbx is refused
if (N2ENGINE_MODEL_UFBX)
    target_compile_definitions(engine_tests PRIVATE N2ENGINE_MODEL_UFBX=1)
endif()

# tests/common has the glTF file builder the model tests share with tests/assetimport
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)

add_test(NAME engine_tests COMMAND engine_tests)
