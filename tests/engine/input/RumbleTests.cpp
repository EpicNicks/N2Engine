#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <tuple>
#include <vector>

#include "engine/input/Rumble.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace std::chrono_literals;

namespace
{
    // Records what reaches the motors; `connected` gamepads have rumble
    struct RecordingOutput final : RumbleOutput
    {
        int connected = 1;
        std::vector<std::tuple<int, float, float>> calls;
        bool Set(const int gamepad, const float low, const float high) override
        {
            calls.emplace_back(gamepad, low, high);
            return gamepad < connected;
        }
    };

    // Installs a RecordingOutput for one test, with no rumble left over on either side
    struct ScopedOutput
    {
        RecordingOutput output;
        RumbleOutput *previous;
        ScopedOutput() : previous(Rumble::SetOutput(&output)) {}
        ~ScopedOutput()
        {
            Rumble::StopAll();
            Rumble::SetOutput(previous);
        }
    };
}

TEST(RumbleTest, TimedRumbleStopsAfterItsRealTime)
{
    ScopedOutput scoped;
    const auto t0 = Rumble::Clock::time_point{} + 100s;
    ASSERT_TRUE(Rumble::Start(0, 0.25f, 0.75f, 0.2, t0));
    ASSERT_EQ(scoped.output.calls.size(), 1u);
    EXPECT_EQ(scoped.output.calls[0], std::make_tuple(0, 0.25f, 0.75f));
    EXPECT_TRUE(Rumble::IsActive(0));

    Rumble::Update(t0 + 150ms);
    EXPECT_TRUE(Rumble::IsActive(0)) << "not before 0.2 s";
    EXPECT_EQ(scoped.output.calls.size(), 1u);

    Rumble::Update(t0 + 200ms);
    EXPECT_FALSE(Rumble::IsActive(0));
    ASSERT_EQ(scoped.output.calls.size(), 2u);
    EXPECT_EQ(scoped.output.calls[1], std::make_tuple(0, 0.0f, 0.0f)) << "stopped with both motors at 0";

    Rumble::Update(t0 + 1s);
    EXPECT_EQ(scoped.output.calls.size(), 2u) << "stopped once";
}

TEST(RumbleTest, UntimedRumbleLastsUntilStop)
{
    ScopedOutput scoped;
    const auto t0 = Rumble::Clock::time_point{} + 100s;
    ASSERT_TRUE(Rumble::Start(0, 1.0f, 0.0f, 0.0, t0));
    Rumble::Update(t0 + 1h);
    EXPECT_TRUE(Rumble::IsActive(0));
    Rumble::Stop(0);
    EXPECT_FALSE(Rumble::IsActive(0));
    EXPECT_EQ(scoped.output.calls.back(), std::make_tuple(0, 0.0f, 0.0f));
}

TEST(RumbleTest, NewStartReplacesTheTimer)
{
    ScopedOutput scoped;
    const auto t0 = Rumble::Clock::time_point{} + 100s;
    ASSERT_TRUE(Rumble::Start(0, 0.25f, 0.75f, 0.2, t0));
    ASSERT_TRUE(Rumble::Start(0, 0.25f, 0.75f, 0.2, t0 + 150ms)); // hit again before the first one ended
    Rumble::Update(t0 + 250ms);
    EXPECT_TRUE(Rumble::IsActive(0)) << "the second rumble runs to 0.35 s";
    Rumble::Update(t0 + 350ms);
    EXPECT_FALSE(Rumble::IsActive(0));
}

TEST(RumbleTest, NoRumbleWhereTheOutputHasNone)
{
    ScopedOutput scoped;
    EXPECT_FALSE(Rumble::Start(1, 0.5f, 0.5f, 1.0)) << "gamepad 1 is not connected";
    EXPECT_FALSE(Rumble::IsActive(1));
    EXPECT_FALSE(Rumble::Start(-1, 0.5f, 0.5f, 1.0));
    EXPECT_FALSE(Rumble::Start(Rumble::MaxGamepads, 0.5f, 0.5f, 1.0));
    const std::size_t calls = scoped.output.calls.size();
    Rumble::Stop(1); // not rumbling: the motors are not touched
    EXPECT_EQ(scoped.output.calls.size(), calls);
}

TEST(RumbleTest, StopAllStopsEveryRumblingPad)
{
    ScopedOutput scoped;
    scoped.output.connected = 2;
    ASSERT_TRUE(Rumble::Start(0, 0.5f, 0.5f, 0.0));
    ASSERT_TRUE(Rumble::Start(1, 0.5f, 0.5f, 0.0));
    Rumble::StopAll();
    EXPECT_FALSE(Rumble::IsActive(0));
    EXPECT_FALSE(Rumble::IsActive(1));
    EXPECT_EQ(scoped.output.calls.size(), 4u);
}

TEST(RumbleLuaTest, SetAndStopFromLua)
{
    ScopedOutput scoped;
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    auto &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result started =
        lua.safe_script("return Input.SetGamepadRumble(0.25, 0.75, 0.2)", sol::script_pass_on_error);
    ASSERT_TRUE(started.valid());
    EXPECT_TRUE(started.get<bool>());
    EXPECT_TRUE(Rumble::IsActive(0));
    const sol::protected_function_result other =
        lua.safe_script("return Input.SetGamepadRumble(1, 1, 0, 3)", sol::script_pass_on_error);
    ASSERT_TRUE(other.valid());
    EXPECT_FALSE(other.get<bool>()) << "gamepad 3 has no rumble";
    const sol::protected_function_result stopped = lua.safe_script("Input.StopGamepadRumble()", sol::script_pass_on_error);
    ASSERT_TRUE(stopped.valid());
    EXPECT_FALSE(Rumble::IsActive(0));
}
