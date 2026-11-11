#include <gtest/gtest.h>

#include <string>

#include "engine/input/Touchscreen.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;

namespace
{
    // The global touchscreen, emptied before and after each test
    struct ScopedTouchscreen
    {
        Touchscreen &screen = Touchscreen::Get();
        ScopedTouchscreen() { screen.Reset(); }
        ~ScopedTouchscreen() { screen.Reset(); }
    };
}

TEST(TouchscreenTest, PhasesOverAFingersLife)
{
    ScopedTouchscreen s;
    s.screen.Down(7, {10.0f, 20.0f});
    EXPECT_EQ(s.screen.GetTouchCount(), 0u) << "published by Update, not before";
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 1u);
    EXPECT_EQ(s.screen.GetTouches()[0].id, 7);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Began);

    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Stationary);

    s.screen.Down(7, {15.0f, 20.0f});
    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Moved);
    EXPECT_FLOAT_EQ(s.screen.GetTouches()[0].position.x, 15.0f);

    s.screen.Up(7, {16.0f, 21.0f});
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 1u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Ended);
    EXPECT_FLOAT_EQ(s.screen.GetTouches()[0].position.x, 16.0f);

    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouchCount(), 0u) << "Ended is reported for one frame";
}

TEST(TouchscreenTest, ATapBetweenFramesIsNotLost)
{
    ScopedTouchscreen s;
    s.screen.Down(1, {1.0f, 1.0f});
    s.screen.Up(1, {1.0f, 1.0f});
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 1u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Began);
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 1u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Ended);
    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouchCount(), 0u);
}

TEST(TouchscreenTest, SeveralFingersAndCancel)
{
    ScopedTouchscreen s;
    s.screen.Down(1, {1.0f, 1.0f});
    s.screen.Down(2, {2.0f, 2.0f});
    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouchCount(), 2u);
    s.screen.Cancel(2);
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 2u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Stationary);
    EXPECT_EQ(s.screen.GetTouches()[1].phase, TouchPhase::Canceled);
    s.screen.CancelAll();
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 1u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Canceled);
    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouchCount(), 0u);
}

TEST(TouchscreenTest, AnIdCanComeBackAfterItEnded)
{
    ScopedTouchscreen s;
    s.screen.Down(3, {0.0f, 0.0f});
    s.screen.Update();
    s.screen.Up(3, {0.0f, 0.0f});
    s.screen.Down(3, {5.0f, 5.0f}); // the browser reuses ids
    s.screen.Update();
    ASSERT_EQ(s.screen.GetTouchCount(), 2u);
    EXPECT_EQ(s.screen.GetTouches()[0].phase, TouchPhase::Ended);
    EXPECT_EQ(s.screen.GetTouches()[1].phase, TouchPhase::Began);
}

TEST(TouchscreenTest, AtMostMaxTouches)
{
    ScopedTouchscreen s;
    for (int i = 0; i < static_cast<int>(Touchscreen::MaxTouches) + 3; ++i)
    {
        s.screen.Down(i, {0.0f, 0.0f});
    }
    s.screen.Update();
    EXPECT_EQ(s.screen.GetTouchCount(), Touchscreen::MaxTouches);
}

TEST(TouchscreenLuaTest, TouchesFromLua)
{
    ScopedTouchscreen s;
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    auto &lua = Scripting::LuaRuntime::Instance().GetState();
    s.screen.Down(4, {30.0f, 40.0f});
    s.screen.Update();
    const sol::protected_function_result r = lua.safe_script(
        "local t = Input.GetTouch(1) return Input.GetTouchCount(), t.id, t.x, t.y, t.phase, Input.GetTouch(2) == nil",
        sol::script_pass_on_error);
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.get<int>(0), 1);
    EXPECT_EQ(r.get<int>(1), 4);
    EXPECT_FLOAT_EQ(r.get<float>(2), 30.0f);
    EXPECT_FLOAT_EQ(r.get<float>(3), 40.0f);
    EXPECT_EQ(r.get<std::string>(4), "Began");
    EXPECT_TRUE(r.get<bool>(5));
}
