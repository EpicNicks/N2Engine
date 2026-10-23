# n2_replay: the headless replay runner of the golden determinism job (netcode C2). The same code runs in engine_tests
# (tests/engine/replay) on every leg, the web one included; this is the command-line form for a desk or a bug report.
add_executable(n2_replay ReplayMain.cpp)
target_link_libraries(n2_replay PRIVATE engine math)
set_target_properties(n2_replay PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)
if (MSVC)
    target_compile_options(n2_replay PRIVATE /W4)
else()
    target_compile_options(n2_replay PRIVATE -Wall -Wextra)
endif()
