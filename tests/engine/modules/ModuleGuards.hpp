#pragma once

// Skips a test that needs a module this build left out (docs/project-modules.html): for a file whose other tests still apply
// to such a build. A file that only tests the module is left out of the build instead (tests/engine/CMakeLists.txt).
#include <gtest/gtest.h>

#include "engine/audio/AudioSystem.hpp"
#include "engine/ui/UISystem.hpp"

#define N2_SKIP_WITHOUT_UI()                                                                                                 \
    if (!::N2Engine::UI::UISystem::IsInBuild())                                                                              \
    GTEST_SKIP() << "the UI module is not in this build (N2ENGINE_USE_UI is OFF)"

#define N2_SKIP_WITHOUT_AUDIO()                                                                                              \
    if (!::N2Engine::Audio::AudioSystem::IsInBuild())                                                                        \
    GTEST_SKIP() << "the audio module is not in this build (N2ENGINE_USE_AUDIO is OFF)"
