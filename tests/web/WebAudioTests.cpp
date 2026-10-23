// The web build's audio under node (stage 4 of issue #155; docs/web-build.html). Node has no WebAudio, so these are the
// "no device" path of the real backend: Emscripten's OpenAL is linked and finds no AudioContext, and the audio system
// has to behave exactly like a machine with no sound card: disabled, quiet, no crash, nothing waiting on a device.
// What needs a browser (sound coming out, the unlock by a gesture, the tab's suspend and resume) is the manual checklist
// (docs/web-checklist.html); the page's unlock script has its own node test (scripts/web-audio-unlock-test.mjs).
#include <gtest/gtest.h>

#include <emscripten.h>

#include <cstdint>

#include "engine/audio/AudioSystem.hpp"

using namespace N2Engine::Audio;

namespace
{
    // The premise of every test here: this run has no AudioContext. (Node 22 does not define one; if a future node does,
    // these tests would be testing something else, so say so rather than fail confusingly.)
    bool RunHasNoWebAudio()
    {
        return EM_ASM_INT({
            return (typeof AudioContext === 'undefined' && typeof webkitAudioContext === 'undefined') ? 1 : 0;
        }) != 0;
    }
}

class WebAudioDegradeTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!RunHasNoWebAudio())
        {
            GTEST_SKIP() << "this node has an AudioContext";
        }
        AudioSystem::Instance().Shutdown();
    }
    void TearDown() override { AudioSystem::Instance().Shutdown(); }
};

TEST_F(WebAudioDegradeTests, DeviceOutputFailsToInitializeAndStaysDisabled)
{
    auto &audio = AudioSystem::Instance();
    EXPECT_FALSE(audio.Initialize());
    EXPECT_FALSE(audio.IsInitialized());
    EXPECT_FALSE(audio.IsLoopback());
    // A second try fails the same way (no leftover half-opened device)
    EXPECT_FALSE(audio.Initialize());
    EXPECT_FALSE(audio.IsInitialized());
}

TEST_F(WebAudioDegradeTests, LoopbackIsNotOfferedByTheWebBackend)
{
    // Emscripten's OpenAL has no ALC_SOFT_loopback: the editor's streamed audio cannot be produced on the web, and
    // the audio system says so instead of pretending
    EXPECT_FALSE(AudioSystem::IsLoopbackSupported());
    auto &audio = AudioSystem::Instance();
    EXPECT_FALSE(audio.Initialize(AudioOutput::Loopback));
    EXPECT_FALSE(audio.IsInitialized());
    EXPECT_EQ(audio.GetOutput(), AudioOutput::Device);
    std::uint8_t buffer[16] = {};
    EXPECT_FALSE(audio.RenderSamples(buffer, 2));
    EXPECT_EQ(audio.GetStreamedFrameCount(), 0u);
}

TEST_F(WebAudioDegradeTests, EverythingIsAQuietNoOpWithoutAnOutput)
{
    auto &audio = AudioSystem::Instance();
    ASSERT_FALSE(audio.Initialize());

    // The "output is suspended" gate is about a context the browser keeps from running; with none there is nothing
    // suspended, and the sound is not "dropped for the unlock" (it is simply not playable)
    EXPECT_FALSE(audio.IsOutputSuspended());
    EXPECT_FALSE(audio.RefuseWhileSuspended());

    EXPECT_FALSE(audio.PlayOneShot(nullptr).IsValid());
    EXPECT_EQ(audio.AcquireSource(), 0u);
    audio.SetMasterVolume(0.25f);
    EXPECT_FLOAT_EQ(audio.GetMasterVolume(), 0.25f); // kept, applied when a context opens
    audio.SetListenerPosition(1.0f, 2.0f, 3.0f);
    audio.SetListenerOrientation(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f);
    audio.Update();
    audio.SetMasterVolume(1.0f);
}
