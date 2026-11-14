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
        # OpenAL: these open OpenAL Soft's own "null" and loopback devices (ALSOFT_DRIVERS=null, ALC_SOFT_loopback), which
        # Emscripten's OpenAL (WebAudio; node has none, so no device opens at all) does not have. The web build's audio
        # is tested by tests/web/WebAudioTests.cpp (the no-device path under node) and by the manual browser checklist
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

# A build without the particles or soft-body module (N2ENGINE_USE_PARTICLES, N2ENGINE_USE_SOFTBODIES): the tests of that
# module's simulation are left out (its code is not in the engine to test), and modules/LeftOutModulesTests.cpp checks what such
# a build does instead. The tests of what stays (the GPU mode and setting, the health rows' mapping) still run.
set(N2ENGINE_PARTICLES_TESTS
    particles/ParticleHeadlessTests.cpp
    particles/ParticleSimulationTests.cpp
    particles/ParticleSystemTests.cpp
    particles/PbdParticleBackendTests.cpp
    physics/PhysXGpuParticlesTests.cpp
    rendering/ParticleRendererTests.cpp
    rendering/SoftwareParticlesGoldenTests.cpp
    scripting/LuaParticleSystemTests.cpp
)
set(N2ENGINE_SOFTBODIES_TESTS
    physics/DeformableCookingTests.cpp
    physics/PhysXGpuSoftBodiesTests.cpp
    scripting/LuaSoftBodyTests.cpp
    softbody/CpuSoftBodyBackendTests.cpp
    softbody/DeformableSoftBodyBackendTests.cpp
    softbody/DeformableSoftBodyComponentTests.cpp
    softbody/SoftBodyComponentTests.cpp
    softbody/SoftBodyHeadlessTests.cpp
    softbody/SoftBodySelectionTests.cpp # its GPU cases use FakePhysicsBackend's soft-body fake, which runs the CPU backend
    softbody/TetrahedraliserTests.cpp
)
# These install fake GPU particle and soft-body drivers into the PhysX backend, which a build without either module refuses
# (GpuProbeTests.cpp stays in every build: only its cases that install a fake driver need both modules, and it guards them)
if(N2ENGINE_PHYSX_AVAILABLE)
    list(APPEND N2ENGINE_SOFTBODIES_TESTS physics/PhysXSoftBodiesBackendTests.cpp)
endif()
# The audio and UI modules (N2ENGINE_USE_AUDIO, N2ENGINE_USE_UI) the same way: the files that only test the module are left out,
# and a test of another subsystem that also needs one skips itself (modules/ModuleGuards.hpp)
set(N2ENGINE_AUDIO_TESTS
    audio/AudioTests.cpp
    audio/LoopbackAudioTests.cpp
)
set(N2ENGINE_UI_TESTS
    rendering/SoftwareImageGoldenTests.cpp
    rendering/SoftwareWorldCanvasGoldenTests.cpp
    scripting/LuaImageSpriteTests.cpp
    scripting/LuaUITests.cpp
    ui/ButtonTests.cpp
    ui/ImageColorEffectsTests.cpp
    ui/ImageCutOutTests.cpp
    ui/UITests.cpp
    ui/UITextCharacterStyleTests.cpp
    ui/UITextOffsetTests.cpp
    ui/UITextTests.cpp
    ui/WorldCanvasTests.cpp
)
foreach(_n2_module PARTICLES SOFTBODIES AUDIO UI)
    foreach(_n2_test IN LISTS N2ENGINE_${_n2_module}_TESTS)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_n2_test}")
            message(FATAL_ERROR "N2ENGINE_${_n2_module}_TESTS names a file that does not exist: ${_n2_test}")
        endif()
        if(NOT N2ENGINE_USE_${_n2_module})
            list(REMOVE_ITEM ENGINE_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_n2_test}")
        endif()
    endforeach()
endforeach()

add_executable(engine_tests ${ENGINE_TEST_SOURCES})

# On this file only, so a build without Box2D recompiles almost nothing else
if(N2ENGINE_USE_BOX2D)
    set_source_files_properties(physics2d/Physics2DTests.cpp physics2d/LuaPhysics2DTests.cpp PROPERTIES COMPILE_DEFINITIONS N2ENGINE_BOX2D_ENABLED=1)
endif()
# The particles and soft-body modules: on the whole program, not per file as Box2D's, because the shared test fakes
# (particles/FakePhysicsBackend.hpp, included by tests of other subsystems too) run those modules' CPU backends.
# modules/LeftOutModulesTests.cpp has tests only for a module that is left out.
foreach(_n2_module PARTICLES SOFTBODIES AUDIO UI)
    if(N2ENGINE_USE_${_n2_module})
        target_compile_definitions(engine_tests PRIVATE N2ENGINE_${_n2_module}_ENABLED=1)
    endif()
endforeach()

target_link_libraries(engine_tests
        PRIVATE
        engine
        n2net # the netcode packet layer and simulator: no engine dependency
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
        N2_ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/engine" # DeterministicMathSourceScanTests scans the sources as text
        N2_MATH_SOURCE_DIR="${CMAKE_SOURCE_DIR}/math"
        N2_GOLDEN_REPLAY_DIR="${CMAKE_SOURCE_DIR}/tests/golden/replays" # the replay files of the golden determinism job (C2)
)

# With the optional FBX/OBJ importer (N2ENGINE_MODEL_UFBX) the model tests also load the committed FBX; without it they
# check that an .fbx is refused
if (N2ENGINE_MODEL_UFBX)
    target_compile_definitions(engine_tests PRIVATE N2ENGINE_MODEL_UFBX=1)
endif()

# tests/common has the glTF file builder the model tests share with tests/assetimport
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
# engine/src: tests of the engine's private headers (input/GlfwInputMapping.hpp: the GLFW device's tables)
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)

add_test(NAME engine_tests COMMAND engine_tests)
