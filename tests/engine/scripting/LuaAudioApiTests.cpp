#include <gtest/gtest.h>

#include <string>

#include "engine/audio/AudioSystem.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The Lua audio API of issue #445 without an audio output (the tests' usual state): the DSP clock, mixer groups and
// the synthesis functions' validation. Playback itself is tested on the loopback device (LoopbackAudioTests.cpp).
namespace
{
    class LuaAudioApiTest : public ::testing::Test
    {
    protected:
        void SetUp() override { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }

        template <typename T>
        T Eval(const std::string &code)
        {
            const sol::protected_function_result result =
                LuaRuntime::Instance().GetState().safe_script("return " + code, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error error = result;
                ADD_FAILURE() << code << ": " << error.what();
                return T{};
            }
            return result.get<T>();
        }

        std::string ErrorOf(const std::string &code)
        {
            const sol::protected_function_result result =
                LuaRuntime::Instance().GetState().safe_script(code, sol::script_pass_on_error);
            if (result.valid())
            {
                return {};
            }
            const sol::error error = result;
            return error.what();
        }
    };
}

TEST_F(LuaAudioApiTest, DspClockIsANumberAndZeroWithoutAnOutput)
{
    EXPECT_TRUE(Eval<bool>("type(Audio.GetDspTime()) == 'number'"));
    EXPECT_TRUE(Eval<bool>("type(Audio.IsSampleAccurate()) == 'boolean'"));
    if (!Audio::AudioSystem::Instance().IsInitialized())
    {
        EXPECT_DOUBLE_EQ(Eval<double>("Audio.GetDspTime()"), 0.0);
        EXPECT_EQ(Eval<int>("Audio.GetSampleRate()"), 0);
    }
}

TEST_F(LuaAudioApiTest, CreateGroupMakesAGroupOnceWithItsSettings)
{
    EXPECT_TRUE(Eval<bool>("Audio.CreateGroup('Lua445Stems', {volume = 0.5, pitch = 20, maxConcurrent = 3})"));
    EXPECT_FALSE(Eval<bool>("Audio.CreateGroup('Lua445Stems')")) << "it exists already";
    EXPECT_TRUE(Eval<bool>("Audio.HasGroup('Lua445Stems')"));
    EXPECT_FLOAT_EQ(Eval<float>("Audio.GetGroupVolume('Lua445Stems')"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("Audio.GetGroupPitch('Lua445Stems')"), Audio::AudioSystem::MaxPitch) << "clamped";
    EXPECT_EQ(Eval<int>("Audio.GetGroupMaxConcurrent('Lua445Stems')"), 3);

    const auto *group = Audio::AudioSystem::Instance().GetMixerGroup("Lua445Stems");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->settings.maxConcurrent, 3u);

    EXPECT_TRUE(Eval<bool>("Audio.CreateGroup('Lua445Plain')"));
    EXPECT_TRUE(Eval<bool>("Audio.GetGroupMaxConcurrent('Lua445Plain') == nil")) << "no limit by default";

    EXPECT_NE(ErrorOf("Audio.CreateGroup('Lua445Bad', {volume = 'loud'})").find("volume"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateGroup('')").find("name"), std::string::npos);
}

TEST_F(LuaAudioApiTest, SetGroupMaxConcurrentSetsAndClearsTheLimit)
{
    ASSERT_TRUE(Eval<bool>("Audio.CreateGroup('Lua445Limit')"));
    Eval<bool>("(function() Audio.SetGroupMaxConcurrent('Lua445Limit', 2) return true end)()");
    EXPECT_EQ(Eval<int>("Audio.GetGroupMaxConcurrent('Lua445Limit')"), 2);
    Eval<bool>("(function() Audio.SetGroupMaxConcurrent('Lua445Limit', nil) return true end)()");
    EXPECT_TRUE(Eval<bool>("Audio.GetGroupMaxConcurrent('Lua445Limit') == nil"));
    Eval<bool>("(function() Audio.SetGroupMaxConcurrent('Lua445Limit', 4) return true end)()");
    EXPECT_EQ(Eval<int>("Audio.GetGroupMaxConcurrent('Lua445Limit')"), 4);
    Eval<bool>("(function() Audio.SetGroupMaxConcurrent('Lua445Limit', 0) return true end)()");
    EXPECT_TRUE(Eval<bool>("Audio.GetGroupMaxConcurrent('Lua445Limit') == nil")) << "0 is no limit";
    EXPECT_TRUE(Eval<bool>("Audio.GetGroupMaxConcurrent('NoSuchGroup445') == nil"));
}

TEST_F(LuaAudioApiTest, CreateToneValidatesItsParameters)
{
    EXPECT_NE(ErrorOf("Audio.CreateTone{waveform = 'kazoo'}").find("waveform"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateTone{frequency = 0}").find("frequency"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateTone{duration = 1000}").find("duration"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateTone{volume = 'x'}").find("volume"), std::string::npos);

    // Valid: an AudioClip of the right length with an output, nil without one
    EXPECT_TRUE(Eval<bool>(
        "(function() local c = Audio.CreateTone{waveform = 'triangle', frequency = 220, duration = 0.1, "
        "attack = 0.01, release = 0.02} return c == nil or c:GetFrameCount() == 4800 end)()"));
}

TEST_F(LuaAudioApiTest, CreateClipValidatesItsSamples)
{
    EXPECT_NE(ErrorOf("Audio.CreateClip({0.1, 'x', 0.3})").find("sample 2"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateClip({0.1, 0.2, 0.3}, 48000, 2)").find("frames"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateClip({}, 48000)").find("no samples"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.CreateClip({0, 0}, 10)").find("sampleRate"), std::string::npos);
    EXPECT_TRUE(Eval<bool>(
        "(function() local c = Audio.CreateClip({0, 0.5, -0.5, 1}, 8000) "
        "return c == nil or (c:GetFrameCount() == 4 and c:GetSampleRate() == 8000) end)()"));
}

TEST_F(LuaAudioApiTest, PlayOneShotAtRejectsSomethingThatIsNotAClip)
{
    EXPECT_NE(ErrorOf("Audio.PlayOneShotAt(42, 1.0)").find("path or an AudioClip"), std::string::npos);
    EXPECT_NE(ErrorOf("Audio.PlayOneShotAt('res://no/such/clip445.ogg', 1.0)").find("clip445"), std::string::npos);
}
