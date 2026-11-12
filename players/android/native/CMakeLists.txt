# libn2game.so, the Android player's native library (build targets stage 8; docs/android.html). Added by the root
# CMakeLists.txt when Gradle configures this repository with the NDK (players/android/app/build.gradle): the GameActivity
# prefab (androidx.games:games-activity) is found through the paths the Android Gradle plugin hands CMake.
# A plain NDK configure (no Gradle, so no prefab) builds the engine for Android without the player: CI's compile check
find_package(game-activity CONFIG QUIET)
if(NOT game-activity_FOUND)
    message(STATUS "Android: no GameActivity prefab (not a Gradle build): libn2game.so is not built, the engine is")
    return()
endif()

add_library(n2game SHARED
    src/AndroidMain.cpp
    src/AndroidHost.cpp
)
target_include_directories(n2game PRIVATE src)
target_link_libraries(n2game PRIVATE
    engine
    renderer
    MathCore::math
    game-activity::game-activity_static
    android
    log
)
set_target_properties(n2game PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
# GameActivity's Java side calls this native entry point by name: keep it although nothing in C++ refers to it
target_link_options(n2game PRIVATE "-u" "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")
# The module check against what the game uses, as the desktop player does on link
n2_check_project_modules(n2game)
