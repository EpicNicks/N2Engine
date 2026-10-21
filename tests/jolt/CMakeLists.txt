# A smoke test of the vendored Jolt Physics library (cmake/jolt/CMakeLists.txt) alone: it links Jolt::Jolt and GoogleTest,
# no engine, math or renderer target, so none of their flags (math's -mavx) reach it and the flags that Jolt::Jolt
# carries (floating-point, instruction set) are the only ones that matter. Added by tests/CMakeLists.txt when
# N2ENGINE_USE_JOLT is ON.
add_executable(jolt_smoke_tests JoltSmokeTests.cpp)

target_link_libraries(jolt_smoke_tests
        PRIVATE
        Jolt::Jolt
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(jolt_smoke_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

add_test(NAME jolt_smoke_tests COMMAND jolt_smoke_tests)
