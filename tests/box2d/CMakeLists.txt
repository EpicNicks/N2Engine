# A smoke test of the vendored Box2D library (cmake/box2d/CMakeLists.txt) alone: it links box2d::box2d and GoogleTest, no
# engine, math or renderer target, so the flags box2d::box2d carries are the only ones that matter. Added by
# tests/CMakeLists.txt when N2ENGINE_USE_BOX2D is ON. shared/determinism.c is upstream's own determinism scene.
add_executable(box2d_smoke_tests
        Box2DSmokeTests.cpp
        ${CMAKE_SOURCE_DIR}/external/box2d/shared/determinism.c
)

target_include_directories(box2d_smoke_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/external/box2d/shared)

target_link_libraries(box2d_smoke_tests
        PRIVATE
        box2d::box2d
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(box2d_smoke_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
)

add_test(NAME box2d_smoke_tests COMMAND box2d_smoke_tests)
