#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioSystem.hpp"

using namespace N2Engine::Audio;

// The loopback device (ALC_SOFT_loopback) only mixes when asked, so these tests drive playback by rendering
// samples: no sound card, no null backend, and no real-time waits.
namespace
{
    constexpr float SineAmplitude = 0.5f;
    constexpr float SineFrequency = 1000.0f;
    /// A rendered RMS below this is silence (int16 output may carry +-1 LSB of dither)
    constexpr double SilenceRms = 1e-3;

    std::uint32_t Frames(double seconds)
    {
        return static_cast<std::uint32_t>(std::lround(seconds * AudioSystem::LoopbackSampleRate));
    }

    /// A mono 16-bit sine at the loopback rate, so the mixer doesn't resample it
    std::shared_ptr<AudioClip> MakeSineClip(double seconds)
    {
        AudioData data;
        data.sampleRate = static_cast<int>(AudioSystem::LoopbackSampleRate);
        data.channels = 1;
        data.bitsPerSample = 16;

        const std::uint32_t frames = Frames(seconds);
        data.samples.resize(static_cast<std::size_t>(frames) * sizeof(std::int16_t));
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const double phase = 2.0 * std::numbers::pi * SineFrequency * i / AudioSystem::LoopbackSampleRate;
            const auto sample = static_cast<std::int16_t>(std::lround(SineAmplitude * std::sin(phase) * 32767.0));
            std::memcpy(data.samples.data() + static_cast<std::size_t>(i) * sizeof(sample), &sample, sizeof(sample));
        }

        auto clip = std::make_shared<AudioClip>();
        if (!clip->CreateFromData(data))
        {
            return nullptr;
        }
        return clip;
    }

    /// Interleaved samples as floats (-1..1), whatever the loopback sample format
    std::vector<float> ToFloats(const std::vector<std::uint8_t> &bytes, SampleFormat format)
    {
        std::vector<float> out;
        if (format == SampleFormat::Float32)
        {
            out.resize(bytes.size() / sizeof(float));
            std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
        }
        else
        {
            out.resize(bytes.size() / sizeof(std::int16_t));
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                std::int16_t sample = 0;
                std::memcpy(&sample, bytes.data() + i * sizeof(sample), sizeof(sample));
                out[i] = static_cast<float>(sample) / 32768.0f;
            }
        }
        return out;
    }

    /// RMS of one channel of interleaved samples, skipping the first skipFrames frames
    double ChannelRms(const std::vector<float> &samples, std::uint32_t channels, std::uint32_t channel,
                      std::size_t skipFrames = 0)
    {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = skipFrames * channels + channel; i < samples.size(); i += channels)
        {
            sum += static_cast<double>(samples[i]) * samples[i];
            ++count;
        }
        return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
    }
}

class LoopbackAudioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        AudioSystem::Instance().Shutdown(); // whatever an earlier test left running
        if (!AudioSystem::IsLoopbackSupported())
        {
            GTEST_SKIP() << "This OpenAL build has no ALC_SOFT_loopback extension; loopback audio can't be tested";
        }
        ASSERT_TRUE(AudioSystem::Instance().Initialize(AudioOutput::Loopback));
        ASSERT_TRUE(AudioSystem::Instance().IsLoopback());
        AudioSystem::Instance().SetMasterVolume(1.0f);
    }

    void TearDown() override
    {
        // Sources stop before the clips they play are released, and both before the context goes
        for (AudioHandle handle : _handles)
        {
            AudioSystem::Instance().Stop(handle);
        }
        _clips.clear();
        AudioSystem::Instance().Shutdown();
    }

    std::shared_ptr<AudioClip> Clip(double seconds)
    {
        auto clip = MakeSineClip(seconds);
        if (clip)
        {
            _clips.push_back(clip);
        }
        return clip;
    }

    AudioHandle Play(const std::shared_ptr<AudioClip> &clip)
    {
        const AudioHandle handle = AudioSystem::Instance().PlayOneShot(clip);
        _handles.push_back(handle);
        return handle;
    }

    /// Renders frames directly (RenderSamples) and returns them as floats
    std::vector<float> Render(std::uint32_t frames)
    {
        auto &audio = AudioSystem::Instance();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(frames) * audio.GetLoopbackFormat().BytesPerFrame());
        EXPECT_TRUE(audio.RenderSamples(bytes.data(), frames));
        return ToFloats(bytes, audio.GetLoopbackFormat().sampleFormat);
    }

    [[nodiscard]] std::uint32_t Channels() const { return AudioSystem::Instance().GetLoopbackFormat().channels; }

private:
    std::vector<AudioHandle> _handles;
    std::vector<std::shared_ptr<AudioClip>> _clips;
};

// ============================================================================
// Device and format
// ============================================================================

TEST_F(LoopbackAudioTest, UsesAFixed48kHzStereoFormat)
{
    auto &audio = AudioSystem::Instance();
    const LoopbackFormat &format = audio.GetLoopbackFormat();

    EXPECT_EQ(audio.GetOutput(), AudioOutput::Loopback);
    EXPECT_EQ(format.sampleRate, 48000u);
    EXPECT_EQ(format.channels, 2u);
    EXPECT_TRUE(format.sampleFormat == SampleFormat::Float32 || format.sampleFormat == SampleFormat::Int16);
    EXPECT_EQ(format.BytesPerFrame(), format.sampleFormat == SampleFormat::Float32 ? 8u : 4u);
    EXPECT_EQ(audio.GetStreamCapacityFrames(), 48000u * AudioSystem::StreamBufferMilliseconds / 1000u);
}

TEST_F(LoopbackAudioTest, InitializeWithTheOtherOutputIsRefused)
{
    auto &audio = AudioSystem::Instance();
    EXPECT_TRUE(audio.Initialize(AudioOutput::Loopback)) << "same output: idempotent";
    EXPECT_FALSE(audio.Initialize(AudioOutput::Device)) << "already running on the loopback device";
    EXPECT_TRUE(audio.IsLoopback());
}

TEST_F(LoopbackAudioTest, ShutdownLeavesLoopbackMode)
{
    auto &audio = AudioSystem::Instance();
    audio.Shutdown();

    EXPECT_FALSE(audio.IsLoopback());
    EXPECT_EQ(audio.GetOutput(), AudioOutput::Device);
    EXPECT_EQ(audio.GetStreamCapacityFrames(), 0u);
    std::vector<std::uint8_t> buffer(64, 0xCD);
    EXPECT_FALSE(audio.RenderSamples(buffer.data(), 4)) << "no loopback device to render";
    EXPECT_EQ(buffer[0], 0xCD);
    audio.AdvanceStream(1.0); // no-op
    EXPECT_EQ(audio.TakeStreamedAudio().frameCount, 0u);
}

TEST(LoopbackAudioUninitializedTest, StreamCallsAreNoOpsWithoutALoopbackDevice)
{
    auto &audio = AudioSystem::Instance();
    audio.Shutdown();
    ASSERT_FALSE(audio.IsLoopback());

    std::vector<std::uint8_t> buffer(64, 0xCD);
    EXPECT_FALSE(audio.RenderSamples(buffer.data(), 4));
    audio.AdvanceStream(0.5);
    EXPECT_EQ(audio.GetStreamedFrameCount(), 0u);
    const StreamedAudio taken = audio.TakeStreamedAudio();
    EXPECT_EQ(taken.frameCount, 0u);
    EXPECT_TRUE(taken.samples.empty());
}

// ============================================================================
// Rendering a known clip
// ============================================================================

TEST_F(LoopbackAudioTest, RenderSamplesWritesExactlyTheRequestedFrames)
{
    auto &audio = AudioSystem::Instance();
    const std::size_t frameBytes = audio.GetLoopbackFormat().BytesPerFrame();
    constexpr std::uint32_t frames = 1000;
    constexpr std::size_t guardBytes = 64;

    std::vector<std::uint8_t> buffer(frames * frameBytes + guardBytes, 0xCD);
    ASSERT_TRUE(audio.RenderSamples(buffer.data(), frames));

    for (std::size_t i = frames * frameBytes; i < buffer.size(); ++i)
    {
        ASSERT_EQ(buffer[i], 0xCD) << "wrote past the requested frames at byte " << i;
    }
}

TEST_F(LoopbackAudioTest, OutputIsSilentBeforeAnythingPlays)
{
    ASSERT_NE(Clip(0.2), nullptr); // loaded but not playing
    const std::vector<float> out = Render(Frames(0.1));

    ASSERT_EQ(out.size(), static_cast<std::size_t>(Frames(0.1)) * Channels());
    EXPECT_LT(ChannelRms(out, Channels(), 0), SilenceRms);
    EXPECT_LT(ChannelRms(out, Channels(), 1), SilenceRms);
}

TEST_F(LoopbackAudioTest, PlayingClipRendersAtTheExpectedLevel)
{
    const auto clip = Clip(0.25);
    ASSERT_NE(clip, nullptr);
    ASSERT_TRUE(Play(clip).IsValid());

    const std::vector<float> out = Render(Frames(0.1));
    ASSERT_EQ(out.size(), static_cast<std::size_t>(Frames(0.1)) * Channels());

    // A non-spatial mono source sits at the listener, and OpenAL Soft spreads it over both channels. The sine's
    // RMS is A/sqrt(2) ~ 0.354; the stereo panning gain depends on OpenAL Soft's panning mode (roughly 0.5..1),
    // so the bounds are loose but exclude silence and clipping.
    const double expected = SineAmplitude / std::numbers::sqrt2;
    for (std::uint32_t channel = 0; channel < Channels(); ++channel)
    {
        const double rms = ChannelRms(out, Channels(), channel, 256); // past the start-up gain ramp
        EXPECT_GT(rms, expected * 0.2) << "channel " << channel << " is (nearly) silent";
        EXPECT_LT(rms, expected * 1.5) << "channel " << channel << " is louder than the clip";
    }
}

TEST_F(LoopbackAudioTest, OutputIsSilentAfterTheClipEnds)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.05);
    ASSERT_NE(clip, nullptr);
    const AudioHandle handle = Play(clip);
    ASSERT_TRUE(handle.IsValid());

    (void)Render(Frames(0.1)); // the 50 ms clip plays out
    EXPECT_FALSE(audio.IsPlaying(handle));

    const std::vector<float> after = Render(Frames(0.05));
    EXPECT_LT(ChannelRms(after, Channels(), 0), SilenceRms);
    EXPECT_LT(ChannelRms(after, Channels(), 1), SilenceRms);
}

TEST_F(LoopbackAudioTest, MasterVolumeScalesTheMix)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.25);
    ASSERT_NE(clip, nullptr);

    ASSERT_TRUE(Play(clip).IsValid());
    const double full = ChannelRms(Render(Frames(0.1)), Channels(), 0, 256);

    audio.SetMasterVolume(0.0f);
    (void)Render(Frames(0.02)); // the gain change ramps in over the next mix
    const double muted = ChannelRms(Render(Frames(0.05)), Channels(), 0);

    EXPECT_GT(full, 0.05);
    EXPECT_LT(muted, SilenceRms);
}

// ============================================================================
// The audio clock only moves when samples are rendered
// ============================================================================

TEST_F(LoopbackAudioTest, NothingFinishesWithoutRendering)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.01);
    ASSERT_NE(clip, nullptr);
    const AudioHandle handle = Play(clip);
    ASSERT_TRUE(handle.IsValid());

    audio.Update();
    EXPECT_TRUE(audio.IsPlaying(handle)) << "no samples rendered, so no time has passed";
    EXPECT_NE(audio.GetOneShotSource(handle), 0u);
}

TEST_F(LoopbackAudioTest, OneShotFinishesAndItsSourceReturnsToThePool)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.05);
    ASSERT_NE(clip, nullptr);
    const AudioHandle handle = Play(clip);
    ASSERT_TRUE(handle.IsValid());
    const ALuint source = audio.GetOneShotSource(handle);
    ASSERT_NE(source, 0u);

    (void)Render(Frames(0.03));
    audio.Update();
    EXPECT_TRUE(audio.IsPlaying(handle)) << "30 ms into a 50 ms clip";

    (void)Render(Frames(0.05));
    audio.Update();
    EXPECT_FALSE(audio.IsPlaying(handle));
    EXPECT_EQ(audio.GetOneShotSource(handle), 0u) << "Update should have forgotten the finished one-shot";
    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 0u);

    // The pool hands out the most recently released source first
    const AudioHandle next = Play(clip);
    ASSERT_TRUE(next.IsValid());
    EXPECT_EQ(audio.GetOneShotSource(next), source) << "the finished one-shot's source was not recycled";
}

// ============================================================================
// Pacing and the bounded stream buffer
// ============================================================================

TEST_F(LoopbackAudioTest, AdvanceStreamRendersByElapsedTimeNotByCall)
{
    auto &audio = AudioSystem::Instance();

    // One second in 60 steps: 48000 frames, however the steps fall
    std::uint64_t total = 0;
    for (int i = 0; i < 60; ++i)
    {
        audio.AdvanceStream(1.0 / 60.0);
        const StreamedAudio taken = audio.TakeStreamedAudio();
        EXPECT_EQ(taken.samples.size(), taken.frameCount * audio.GetLoopbackFormat().BytesPerFrame());
        EXPECT_EQ(taken.droppedFrames, 0u);
        total += taken.frameCount;
    }
    EXPECT_NEAR(static_cast<double>(total), 48000.0, 1.0);

    // Steps shorter than a frame accumulate rather than being lost
    total = 0;
    for (int i = 0; i < 1000; ++i)
    {
        audio.AdvanceStream(1.0 / 96000.0); // half a frame
        total += audio.TakeStreamedAudio().frameCount;
    }
    EXPECT_NEAR(static_cast<double>(total), 500.0, 1.0);
}

TEST_F(LoopbackAudioTest, AdvanceStreamIgnoresNonPositiveTime)
{
    auto &audio = AudioSystem::Instance();
    audio.AdvanceStream(0.0);
    audio.AdvanceStream(-1.0);
    audio.AdvanceStream(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(audio.GetStreamedFrameCount(), 0u);
}

TEST_F(LoopbackAudioTest, AdvanceStreamCapsOneStep)
{
    auto &audio = AudioSystem::Instance();
    audio.AdvanceStream(5.0); // e.g. after a debugger pause

    const StreamedAudio taken = audio.TakeStreamedAudio();
    const auto rendered = static_cast<std::uint64_t>(AudioSystem::MaxStreamAdvanceSeconds * 48000.0);
    EXPECT_EQ(taken.frameCount, audio.GetStreamCapacityFrames());
    EXPECT_EQ(static_cast<std::uint64_t>(taken.frameCount) + taken.droppedFrames, rendered)
        << "one step renders at most MaxStreamAdvanceSeconds";
}

TEST_F(LoopbackAudioTest, StreamBufferStaysBoundedWithoutAReader)
{
    auto &audio = AudioSystem::Instance();
    const std::uint32_t capacity = audio.GetStreamCapacityFrames();
    ASSERT_EQ(capacity, 12000u); // 250 ms at 48 kHz

    // Ten seconds of frames and nobody draining the buffer
    for (int i = 0; i < 600; ++i)
    {
        audio.AdvanceStream(1.0 / 60.0);
        audio.Update();
        ASSERT_LE(audio.GetStreamedFrameCount(), capacity) << "after frame " << i;
    }

    const StreamedAudio taken = audio.TakeStreamedAudio();
    EXPECT_EQ(taken.frameCount, capacity);
    EXPECT_EQ(taken.samples.size(), static_cast<std::size_t>(capacity) * audio.GetLoopbackFormat().BytesPerFrame());
    EXPECT_NEAR(static_cast<double>(taken.droppedFrames), 480000.0 - capacity, 1.0);
    EXPECT_EQ(audio.GetStreamedFrameCount(), 0u) << "taking empties the buffer";
    EXPECT_EQ(audio.TakeStreamedAudio().droppedFrames, 0u) << "the dropped count resets with each take";
}

TEST_F(LoopbackAudioTest, OneShotsFinishAndRecycleWhileNobodyReadsTheStream)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.1);
    ASSERT_NE(clip, nullptr);
    const AudioHandle handle = Play(clip);
    ASSERT_TRUE(handle.IsValid());

    // Half a second of engine frames, as the editor host does with no client connected
    for (int i = 0; i < 30; ++i)
    {
        audio.AdvanceStream(1.0 / 60.0);
        audio.Update();
    }

    EXPECT_FALSE(audio.IsPlaying(handle));
    EXPECT_EQ(audio.GetOneShotSource(handle), 0u);
    EXPECT_LE(audio.GetStreamedFrameCount(), audio.GetStreamCapacityFrames());
}

TEST_F(LoopbackAudioTest, StreamBufferKeepsTheNewestAudioInOrder)
{
    auto &audio = AudioSystem::Instance();
    const auto clip = Clip(0.6);
    ASSERT_NE(clip, nullptr);
    ASSERT_TRUE(Play(clip).IsValid());

    // 7 ms steps (336 frames) don't divide the 12000-frame buffer, so writes wrap around its end. 0.35 s
    // overfills it, so the oldest audio is discarded and the rest must come out oldest first.
    for (int i = 0; i < 50; ++i)
    {
        audio.AdvanceStream(0.007);
    }
    const StreamedAudio taken = audio.TakeStreamedAudio();
    ASSERT_EQ(taken.frameCount, audio.GetStreamCapacityFrames());
    EXPECT_GT(taken.droppedFrames, 0u);

    const std::vector<float> out = ToFloats(taken.samples, audio.GetLoopbackFormat().sampleFormat);
    EXPECT_GT(ChannelRms(out, Channels(), 0), 0.05) << "the clip is still playing";

    // A 1 kHz sine at 48 kHz moves at most ~0.13 * amplitude per frame; splicing chunks out of order
    // would jump by a large fraction of a period
    float largestStep = 0.0f;
    for (std::size_t i = Channels(); i < out.size(); i += Channels())
    {
        largestStep = std::max(largestStep, std::abs(out[i] - out[i - Channels()]));
    }
    EXPECT_LT(largestStep, 0.15f) << "the stream has a discontinuity: chunks came out of order";
}
