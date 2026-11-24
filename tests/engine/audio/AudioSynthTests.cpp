#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "engine/audio/AudioSynth.hpp"

using namespace N2Engine::Audio;

// The tone generator and the PCM converter (issue #445): pure functions, no audio device, so they run in every build
namespace
{
    std::vector<std::int16_t> Samples(const AudioData &data)
    {
        std::vector<std::int16_t> out(data.samples.size() / sizeof(std::int16_t));
        std::memcpy(out.data(), data.samples.data(), out.size() * sizeof(std::int16_t));
        return out;
    }

    /// A tone with no envelope (full level throughout), so the samples are the bare waveform
    ToneParams Flat(const Waveform waveform, const double frequency, const double duration)
    {
        return ToneParams{.waveform = waveform,
                          .frequency = frequency,
                          .duration = duration,
                          .volume = 1.0,
                          .attack = 0.0,
                          .decay = 0.0,
                          .sustain = 1.0,
                          .release = 0.0,
                          .sampleRate = 48000};
    }
}

TEST(AudioSynthTest, ToneLengthIsDurationTimesRate)
{
    for (const double duration : {0.001, 0.25, 1.0, 2.5})
    {
        AudioData data;
        ASSERT_FALSE(GenerateTone(Flat(Waveform::Sine, 440.0, duration), data).has_value());
        EXPECT_EQ(data.sampleRate, 48000);
        EXPECT_EQ(data.channels, 1);
        EXPECT_EQ(data.bitsPerSample, 16);
        EXPECT_EQ(Samples(data).size(), static_cast<std::size_t>(std::llround(duration * 48000.0))) << duration;
    }

    ToneParams at22k = Flat(Waveform::Square, 440.0, 0.5);
    at22k.sampleRate = 22050;
    AudioData data;
    ASSERT_FALSE(GenerateTone(at22k, data).has_value());
    EXPECT_EQ(Samples(data).size(), 11025u);
}

TEST(AudioSynthTest, SineAtAQuarterOfTheRateIsZeroPeakZeroTrough)
{
    AudioData data;
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Sine, 12000.0, 0.01), data).has_value());
    const auto samples = Samples(data);
    for (std::size_t i = 0; i + 4 <= samples.size(); i += 4)
    {
        EXPECT_EQ(samples[i], 0);
        EXPECT_EQ(samples[i + 1], 32767);
        EXPECT_EQ(samples[i + 2], 0);
        EXPECT_EQ(samples[i + 3], -32767);
    }
}

TEST(AudioSynthTest, SquareSawAndTriangleHaveTheirShapes)
{
    // 1 kHz at 48 kHz: 48 samples a cycle
    AudioData square;
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Square, 1000.0, 0.01), square).has_value());
    const auto sq = Samples(square);
    for (std::size_t i = 0; i < 48; ++i)
    {
        EXPECT_EQ(sq[i], i < 24 ? 32767 : -32767) << i;
        EXPECT_EQ(sq[i + 48], sq[i]) << "periodic at " << i;
    }

    AudioData saw;
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Saw, 1000.0, 0.01), saw).has_value());
    const auto sw = Samples(saw);
    EXPECT_EQ(sw[0], 0);
    for (std::size_t i = 1; i < 24; ++i)
    {
        EXPECT_GT(sw[i], sw[i - 1]) << "rises through the first half, at " << i;
    }
    EXPECT_EQ(sw[24], -32767) << "wraps to -1 halfway";
    EXPECT_EQ(sw[12], std::lround(0.5 * 32767.0));

    AudioData triangle;
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Triangle, 1000.0, 0.01), triangle).has_value());
    const auto tr = Samples(triangle);
    EXPECT_EQ(tr[0], 0);
    EXPECT_EQ(tr[12], 32767) << "peak a quarter of the way";
    EXPECT_EQ(tr[24], 0);
    EXPECT_EQ(tr[36], -32767) << "trough three quarters of the way";
    EXPECT_EQ(tr[6], std::lround(0.5 * 32767.0));
}

TEST(AudioSynthTest, NoiseIsDeterministicBoundedAndCentred)
{
    AudioData first;
    AudioData second;
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Noise, 1000.0, 0.5), first).has_value());
    ASSERT_FALSE(GenerateTone(Flat(Waveform::Noise, 1000.0, 0.5), second).has_value());
    EXPECT_EQ(first.samples, second.samples) << "the same parameters make the same noise";

    ToneParams otherSeed = Flat(Waveform::Noise, 1000.0, 0.5);
    otherSeed.seed = 7;
    AudioData third;
    ASSERT_FALSE(GenerateTone(otherSeed, third).has_value());
    EXPECT_NE(first.samples, third.samples);

    const auto samples = Samples(first);
    double sum = 0.0;
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        sum += samples[i] / 32767.0;
        distinct += i > 0 && samples[i] != samples[i - 1] ? 1u : 0u;
    }
    EXPECT_NEAR(sum / static_cast<double>(samples.size()), 0.0, 0.02);
    EXPECT_GT(distinct, samples.size() * 9 / 10);
}

TEST(AudioSynthTest, EnvelopeRampsUpHoldsSustainAndReleasesToZero)
{
    // Square wave, so every sample's magnitude is the envelope times the volume
    ToneParams params = Flat(Waveform::Square, 100.0, 1.0);
    params.attack = 0.1;  // 4800 samples
    params.decay = 0.1;   // to sustain by 9600
    params.sustain = 0.5;
    params.release = 0.2; // the last 9600 samples
    AudioData data;
    ASSERT_FALSE(GenerateTone(params, data).has_value());
    const auto samples = Samples(data);
    ASSERT_EQ(samples.size(), 48000u);

    const auto level = [&](std::size_t i) { return std::abs(samples[i]) / 32767.0; };
    EXPECT_EQ(samples[0], 0) << "attack starts from silence";
    EXPECT_NEAR(level(2400), 0.5, 1e-3) << "halfway up the attack";
    EXPECT_NEAR(level(4800), 1.0, 1e-3) << "the attack's peak";
    EXPECT_NEAR(level(7200), 0.75, 1e-3) << "halfway down the decay";
    EXPECT_NEAR(level(20000), 0.5, 1e-3) << "sustain";
    EXPECT_NEAR(level(38399), 0.5, 1e-3) << "sustain until the release";
    EXPECT_LT(level(43200), 0.3) << "releasing";
    EXPECT_EQ(samples.back(), 0) << "the release ends at 0";
}

TEST(AudioSynthTest, EnvelopeLongerThanTheNoteIsScaledToFit)
{
    ToneParams params = Flat(Waveform::Square, 100.0, 0.1);
    params.attack = 0.2;
    params.release = 0.2; // attack + release = 4x the note: both scale to half the note
    AudioData data;
    ASSERT_FALSE(GenerateTone(params, data).has_value());
    const auto samples = Samples(data);
    ASSERT_EQ(samples.size(), 4800u);
    EXPECT_EQ(samples.front(), 0);
    EXPECT_EQ(samples.back(), 0);
    EXPECT_NEAR(std::abs(samples[1200]) / 32767.0, 0.5, 1e-3);
}

TEST(AudioSynthTest, InvalidToneParametersAreRefused)
{
    const auto refused = [](ToneParams params)
    {
        AudioData data;
        const auto error = GenerateTone(params, data);
        return error.has_value() && data.samples.empty();
    };
    const ToneParams good = Flat(Waveform::Sine, 440.0, 0.5);

    ToneParams p = good;
    p.frequency = 0.0;
    EXPECT_TRUE(refused(p));
    p.frequency = 30000.0; // above Nyquist at 48 kHz
    EXPECT_TRUE(refused(p));
    p = good;
    p.duration = 0.0;
    EXPECT_TRUE(refused(p));
    p.duration = MaxSynthSeconds + 1.0;
    EXPECT_TRUE(refused(p));
    p.duration = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(refused(p));
    p = good;
    p.volume = 1.5;
    EXPECT_TRUE(refused(p));
    p = good;
    p.sustain = -0.1;
    EXPECT_TRUE(refused(p));
    p = good;
    p.attack = -1.0;
    EXPECT_TRUE(refused(p));
    p = good;
    p.sampleRate = 1000;
    EXPECT_TRUE(refused(p));
}

TEST(AudioSynthTest, WaveformNames)
{
    EXPECT_EQ(WaveformFromString("sine"), Waveform::Sine);
    EXPECT_EQ(WaveformFromString("square"), Waveform::Square);
    EXPECT_EQ(WaveformFromString("saw"), Waveform::Saw);
    EXPECT_EQ(WaveformFromString("triangle"), Waveform::Triangle);
    EXPECT_EQ(WaveformFromString("noise"), Waveform::Noise);
    EXPECT_FALSE(WaveformFromString("Sine").has_value());
    EXPECT_FALSE(WaveformFromString("").has_value());
}

TEST(AudioSynthTest, PcmFromFloatsConvertsAndClamps)
{
    const std::vector<float> mono{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -3.0f};
    AudioData data;
    ASSERT_FALSE(PcmFromFloats(mono, 44100, 1, data).has_value());
    EXPECT_EQ(data.sampleRate, 44100);
    EXPECT_EQ(data.channels, 1);
    const auto samples = Samples(data);
    ASSERT_EQ(samples.size(), mono.size());
    EXPECT_EQ(samples[0], 0);
    EXPECT_EQ(samples[1], std::lround(0.5 * 32767.0));
    EXPECT_EQ(samples[2], -std::lround(0.5 * 32767.0));
    EXPECT_EQ(samples[3], 32767);
    EXPECT_EQ(samples[4], -32767);
    EXPECT_EQ(samples[5], 32767) << "clamped";
    EXPECT_EQ(samples[6], -32767) << "clamped";

    const std::vector<float> stereo{0.25f, -0.25f, 0.5f, -0.5f};
    ASSERT_FALSE(PcmFromFloats(stereo, 48000, 2, data).has_value());
    EXPECT_EQ(data.channels, 2);
    EXPECT_EQ(Samples(data).size(), 4u);
}

TEST(AudioSynthTest, InvalidPcmIsRefused)
{
    AudioData data;
    EXPECT_TRUE(PcmFromFloats({}, 48000, 1, data).has_value()) << "no samples";
    const std::vector<float> three{0.1f, 0.2f, 0.3f};
    EXPECT_TRUE(PcmFromFloats(three, 48000, 2, data).has_value()) << "not whole stereo frames";
    EXPECT_TRUE(PcmFromFloats(three, 48000, 3, data).has_value()) << "3 channels";
    EXPECT_TRUE(PcmFromFloats(three, 100, 1, data).has_value()) << "rate too low";
    const std::vector<float> nan{0.0f, std::numeric_limits<float>::quiet_NaN()};
    EXPECT_TRUE(PcmFromFloats(nan, 48000, 1, data).has_value()) << "NaN";
    const std::vector<float> tooLong(static_cast<std::size_t>(MaxSynthSeconds * 8000) + 1, 0.0f);
    EXPECT_TRUE(PcmFromFloats(tooLong, 8000, 1, data).has_value()) << "longer than MaxSynthSeconds";
    EXPECT_TRUE(data.samples.empty()) << "nothing written on refusal";
}
