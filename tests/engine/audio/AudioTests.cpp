#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <AL/al.h>

#include "engine/GameObjectScene.hpp" // pulls in the AddComponent template definitions
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioLoaders.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioSystem.hpp"

using namespace N2Engine;
using namespace N2Engine::Audio;
namespace fs = std::filesystem;

namespace
{
    // Route OpenAL Soft to its silent "null" backend so tests need no sound device.
    // Must run before the first ALC call, which is when OpenAL Soft reads its config.
    void UseNullAudioBackend()
    {
#ifdef _WIN32
        _putenv_s("ALSOFT_DRIVERS", "null");
#else
        setenv("ALSOFT_DRIVERS", "null", 1);
#endif
    }

    template <typename T>
    void WriteLE(std::ofstream &out, T value)
    {
        out.write(reinterpret_cast<const char *>(&value), sizeof(T));
    }

    // Writes a PCM WAV file containing a square wave
    void WriteWav(const fs::path &path, int sampleRate, int channels, int bitsPerSample, int frameCount)
    {
        const int bytesPerSample = bitsPerSample / 8;
        const auto dataSize = static_cast<std::uint32_t>(frameCount * channels * bytesPerSample);

        std::ofstream out(path, std::ios::binary);
        out.write("RIFF", 4);
        WriteLE<std::uint32_t>(out, 36 + dataSize);
        out.write("WAVE", 4);

        out.write("fmt ", 4);
        WriteLE<std::uint32_t>(out, 16);
        WriteLE<std::uint16_t>(out, 1); // PCM
        WriteLE<std::uint16_t>(out, static_cast<std::uint16_t>(channels));
        WriteLE<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate));
        WriteLE<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate * channels * bytesPerSample));
        WriteLE<std::uint16_t>(out, static_cast<std::uint16_t>(channels * bytesPerSample));
        WriteLE<std::uint16_t>(out, static_cast<std::uint16_t>(bitsPerSample));

        out.write("data", 4);
        WriteLE<std::uint32_t>(out, dataSize);
        for (int frame = 0; frame < frameCount; ++frame)
        {
            const bool high = (frame / 50) % 2 == 0;
            for (int ch = 0; ch < channels; ++ch)
            {
                if (bitsPerSample == 8)
                {
                    WriteLE<std::uint8_t>(out, static_cast<std::uint8_t>(high ? 192 : 64));
                }
                else
                {
                    WriteLE<std::int16_t>(out, static_cast<std::int16_t>(high ? 8000 : -8000));
                }
            }
        }
    }

    // Raw 16-bit PCM data, bypassing the file loaders
    AudioData MakeAudioData(int sampleRate, int channels, int frameCount)
    {
        AudioData data;
        data.sampleRate = sampleRate;
        data.channels = channels;
        data.bitsPerSample = 16;
        data.samples.resize(static_cast<std::size_t>(frameCount * channels) * sizeof(std::int16_t), 0);
        return data;
    }

    std::shared_ptr<AudioClip> MakeClip(float seconds, int sampleRate = 44100)
    {
        auto clip = std::make_shared<AudioClip>();
        const int frames = static_cast<int>(seconds * static_cast<float>(sampleRate));
        if (!clip->CreateFromData(MakeAudioData(sampleRate, 1, frames)))
        {
            return nullptr;
        }
        return clip;
    }

    // Polls until the condition holds or the timeout expires; playback runs on OpenAL's mixer thread
    bool WaitUntil(const std::function<bool()> &condition,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    }

    ALint SourceState(ALuint source)
    {
        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        return state;
    }
}

// ============================================================================
// WAV loader (no OpenAL context needed)
// ============================================================================

class WAVLoaderTest : public ::testing::Test
{
protected:
    fs::path _dir;

    void SetUp() override
    {
        _dir = fs::temp_directory_path() / "n2engine_audio_tests";
        fs::create_directories(_dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }
};

TEST_F(WAVLoaderTest, LoadsMono16BitFile)
{
    const auto path = _dir / "mono16.wav";
    WriteWav(path, 22050, 1, 16, 1000);

    AudioData data = WAVLoader{}.Load(path);

    EXPECT_EQ(data.sampleRate, 22050);
    EXPECT_EQ(data.channels, 1);
    EXPECT_EQ(data.bitsPerSample, 16);
    EXPECT_EQ(data.samples.size(), 1000u * sizeof(std::int16_t));
}

TEST_F(WAVLoaderTest, LoadsStereo16BitFile)
{
    const auto path = _dir / "stereo16.wav";
    WriteWav(path, 44100, 2, 16, 500);

    AudioData data = WAVLoader{}.Load(path);

    EXPECT_EQ(data.sampleRate, 44100);
    EXPECT_EQ(data.channels, 2);
    EXPECT_EQ(data.samples.size(), 500u * 2u * sizeof(std::int16_t));
}

TEST_F(WAVLoaderTest, Converts8BitTo16Bit)
{
    const auto path = _dir / "mono8.wav";
    WriteWav(path, 8000, 1, 8, 400);

    AudioData data = WAVLoader{}.Load(path);

    EXPECT_EQ(data.bitsPerSample, 16);
    EXPECT_EQ(data.samples.size(), 400u * sizeof(std::int16_t));
}

TEST_F(WAVLoaderTest, PreservesSampleValues)
{
    const auto path = _dir / "values.wav";
    WriteWav(path, 44100, 1, 16, 100);

    AudioData data = WAVLoader{}.Load(path);
    ASSERT_EQ(data.samples.size(), 100u * sizeof(std::int16_t));

    const auto *samples = reinterpret_cast<const std::int16_t *>(data.samples.data());
    EXPECT_EQ(samples[0], 8000);   // first half-period is high
    EXPECT_EQ(samples[50], -8000); // second half-period is low
}

TEST_F(WAVLoaderTest, MissingFileReturnsEmptyData)
{
    AudioData data = WAVLoader{}.Load(_dir / "does_not_exist.wav");

    EXPECT_TRUE(data.samples.empty());
}

TEST_F(WAVLoaderTest, InvalidFileReturnsEmptyData)
{
    const auto path = _dir / "garbage.wav";
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a wav file";
    }

    AudioData data = WAVLoader{}.Load(path);

    EXPECT_TRUE(data.samples.empty());
}

// ============================================================================
// Shared fixture for everything that needs an OpenAL context
// ============================================================================

class AudioContextTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        UseNullAudioBackend();
    }

    void SetUp() override
    {
        ASSERT_TRUE(AudioSystem::Instance().Initialize());
        AudioSystem::Instance().SetMasterVolume(1.0f);
    }

    void TearDown() override
    {
        // A buffer can't be deleted while a source still plays it, and clips and
        // sources must be released while the context is still alive
        for (AudioHandle handle : _oneShots)
        {
            AudioSystem::Instance().Stop(handle);
        }
        ReleaseResources();
        AudioSystem::Instance().Shutdown();
    }

    virtual void ReleaseResources() {}

    // PlayOneShot that stops the sound again on teardown
    AudioHandle PlayOneShot(const std::shared_ptr<AudioClip> &clip, const PlaybackParams &params = {})
    {
        AudioHandle handle = AudioSystem::Instance().PlayOneShot(clip, params);
        _oneShots.push_back(handle);
        return handle;
    }

private:
    std::vector<AudioHandle> _oneShots;
};

// ============================================================================
// AudioSystem without initialization (headless mode, or no audio device)
// ============================================================================

TEST(AudioSystemUninitializedTest, CallsAreSafeNoOps)
{
    auto &audio = AudioSystem::Instance();
    audio.Shutdown(); // make sure no earlier test left it running
    EXPECT_FALSE(audio.IsInitialized());

    audio.Update();
    audio.Stop(AudioHandle{1});
    audio.Shutdown(); // double shutdown is fine

    EXPECT_FALSE(audio.IsPlaying(AudioHandle{1}));
    EXPECT_FALSE(audio.PlayOneShot(std::make_shared<AudioClip>()).IsValid());
    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 0u);
}

// ============================================================================
// AudioClip
// ============================================================================

class AudioClipTest : public AudioContextTest
{
};

TEST_F(AudioClipTest, CreateFromValidDataLoadsBuffer)
{
    AudioClip clip;

    ASSERT_TRUE(clip.CreateFromData(MakeAudioData(44100, 1, 44100)));

    EXPECT_TRUE(clip.IsLoaded());
    EXPECT_NE(clip.GetBuffer(), 0u);
    EXPECT_EQ(clip.GetSampleRate(), 44100);
    EXPECT_EQ(clip.GetChannels(), 1);
}

TEST_F(AudioClipTest, DurationMatchesSampleCount)
{
    AudioClip mono;
    ASSERT_TRUE(mono.CreateFromData(MakeAudioData(44100, 1, 22050)));
    EXPECT_NEAR(mono.GetDuration(), 0.5f, 1e-4f);

    AudioClip stereo;
    ASSERT_TRUE(stereo.CreateFromData(MakeAudioData(48000, 2, 96000)));
    EXPECT_NEAR(stereo.GetDuration(), 2.0f, 1e-4f);
}

TEST_F(AudioClipTest, EmptyDataFails)
{
    AudioClip clip;
    AudioData data;
    data.sampleRate = 44100;
    data.channels = 1;

    EXPECT_FALSE(clip.CreateFromData(data));
    EXPECT_FALSE(clip.IsLoaded());
}

TEST_F(AudioClipTest, UnsupportedChannelCountFails)
{
    AudioClip clip;

    EXPECT_FALSE(clip.CreateFromData(MakeAudioData(44100, 6, 100)));
    EXPECT_FALSE(clip.IsLoaded());
}

TEST_F(AudioClipTest, UnloadResetsState)
{
    AudioClip clip;
    ASSERT_TRUE(clip.CreateFromData(MakeAudioData(44100, 2, 1000)));

    clip.Unload();

    EXPECT_FALSE(clip.IsLoaded());
    EXPECT_EQ(clip.GetBuffer(), 0u);
    EXPECT_EQ(clip.GetDuration(), 0.0f);
    EXPECT_EQ(clip.GetSampleRate(), 0);
    EXPECT_EQ(clip.GetChannels(), 0);
}

TEST_F(AudioClipTest, RecreatingReplacesPreviousBuffer)
{
    AudioClip clip;
    ASSERT_TRUE(clip.CreateFromData(MakeAudioData(44100, 1, 1000)));
    const ALuint first = clip.GetBuffer();

    ASSERT_TRUE(clip.CreateFromData(MakeAudioData(22050, 2, 1000)));

    // The old buffer is either deleted or its name was reused for the new one
    EXPECT_TRUE(first == clip.GetBuffer() || alIsBuffer(first) == AL_FALSE)
        << "previous buffer was leaked";
    EXPECT_EQ(clip.GetSampleRate(), 22050);
    EXPECT_EQ(clip.GetChannels(), 2);
}

TEST_F(AudioClipTest, StaleOpenALErrorDoesNotFailCreation)
{
    // Leave an error pending (querying a source that doesn't exist)
    ALint state = 0;
    alGetSourcei(0xDEADu, AL_SOURCE_STATE, &state);

    AudioClip clip;
    EXPECT_TRUE(clip.CreateFromData(MakeAudioData(44100, 1, 100)));
}

// ============================================================================
// AudioSystem
// ============================================================================

class AudioSystemTest : public AudioContextTest
{
protected:
    std::shared_ptr<AudioClip> _shortClip;
    std::shared_ptr<AudioClip> _longClip;

    void SetUp() override
    {
        AudioContextTest::SetUp();
        _shortClip = MakeClip(0.05f);
        _longClip = MakeClip(5.0f);
        ASSERT_NE(_shortClip, nullptr);
        ASSERT_NE(_longClip, nullptr);
    }

    void ReleaseResources() override
    {
        _shortClip.reset();
        _longClip.reset();
    }
};

TEST_F(AudioSystemTest, InitializeIsIdempotent)
{
    EXPECT_TRUE(AudioSystem::Instance().Initialize());
    EXPECT_TRUE(AudioSystem::Instance().Initialize());
    EXPECT_TRUE(AudioSystem::Instance().IsInitialized());
}

TEST_F(AudioSystemTest, IsInitializedTracksShutdown)
{
    auto &audio = AudioSystem::Instance();
    ASSERT_TRUE(audio.IsInitialized());

    audio.Shutdown();
    EXPECT_FALSE(audio.IsInitialized());

    ASSERT_TRUE(audio.Initialize());
    EXPECT_TRUE(audio.IsInitialized());
}

TEST_F(AudioSystemTest, CreatesDefaultMixerGroups)
{
    auto &audio = AudioSystem::Instance();

    for (const char *name : {"Master", "Music", "SFX", "UI", "Voice", "Ambient"})
    {
        EXPECT_NE(audio.GetMixerGroup(name), nullptr) << name;
    }
    EXPECT_EQ(audio.GetMixerGroup("DoesNotExist"), nullptr);

    EXPECT_EQ(audio.GetMixerGroup("Music")->settings.maxConcurrent, 2u);
    EXPECT_EQ(audio.GetMixerGroup("Voice")->settings.maxConcurrent, 4u);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("UI")->settings.maxSourceVolume, 0.8f);
    EXPECT_FALSE(audio.GetMixerGroup("SFX")->settings.maxConcurrent.has_value());
}

TEST_F(AudioSystemTest, ReinitializeResetsMixerGroups)
{
    auto &audio = AudioSystem::Instance();
    audio.SetGroupVolume("SFX", 0.1f);
    audio.CreateMixerGroup("Custom");

    audio.Shutdown();
    ASSERT_TRUE(audio.Initialize());

    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.volume, 1.0f);
}

TEST_F(AudioSystemTest, CreateMixerGroupUsesSettings)
{
    auto &audio = AudioSystem::Instance();
    audio.CreateMixerGroup("Footsteps", {.volume = 0.5f, .maxConcurrent = 3u});

    const auto *group = audio.GetMixerGroup("Footsteps");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->name, "Footsteps");
    EXPECT_FLOAT_EQ(group->settings.volume, 0.5f);
    EXPECT_EQ(group->settings.maxConcurrent, 3u);
}

TEST_F(AudioSystemTest, GroupSettersClampValues)
{
    auto &audio = AudioSystem::Instance();

    audio.SetGroupVolume("SFX", 1.5f);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.volume, 1.0f);
    audio.SetGroupVolume("SFX", -1.0f);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.volume, 0.0f);

    audio.SetGroupMaxVolume("SFX", 2.0f);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.maxSourceVolume, 1.0f);

    audio.SetGroupPitch("SFX", 10.0f);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.pitch, 2.0f);
    audio.SetGroupPitch("SFX", 0.1f);
    EXPECT_FLOAT_EQ(audio.GetMixerGroup("SFX")->settings.pitch, 0.5f);
}

TEST_F(AudioSystemTest, GroupSettersIgnoreUnknownGroups)
{
    auto &audio = AudioSystem::Instance();

    audio.SetGroupVolume("Nope", 0.5f);
    audio.SetGroupMuted("Nope", true);
    audio.SetGroupMaxConcurrent("Nope", 1);

    EXPECT_EQ(audio.GetMixerGroup("Nope"), nullptr);
}

TEST_F(AudioSystemTest, MasterVolumeClamps)
{
    auto &audio = AudioSystem::Instance();

    audio.SetMasterVolume(3.0f);
    EXPECT_FLOAT_EQ(audio.GetMasterVolume(), 1.0f);

    audio.SetMasterVolume(-3.0f);
    EXPECT_FLOAT_EQ(audio.GetMasterVolume(), 0.0f);
}

TEST_F(AudioSystemTest, ComputeFinalVolumeAppliesGroupSettings)
{
    auto &audio = AudioSystem::Instance();

    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(0.7f, "SFX"), 0.7f);

    // UI caps individual sources at 0.8
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(1.0f, "UI"), 0.8f);
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(0.5f, "UI"), 0.5f);

    // Group volume scales after the cap
    audio.SetGroupVolume("UI", 0.5f);
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(1.0f, "UI"), 0.4f);

    // Unknown groups pass the source volume through
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(0.3f, "Unknown"), 0.3f);
}

TEST_F(AudioSystemTest, MutedGroupIsSilent)
{
    auto &audio = AudioSystem::Instance();

    audio.SetGroupMuted("SFX", true);
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(1.0f, "SFX"), 0.0f);

    audio.SetGroupMuted("SFX", false);
    EXPECT_FLOAT_EQ(audio.ComputeFinalVolume(1.0f, "SFX"), 1.0f);
}

TEST_F(AudioSystemTest, MasterVolumeIsAppliedOnce)
{
    auto &audio = AudioSystem::Instance();
    audio.SetMasterVolume(0.5f);

    ALfloat listenerGain = 0.0f;
    alGetListenerf(AL_GAIN, &listenerGain);

    // Effective loudness is source gain * listener gain; master must contribute exactly once
    const float effective = audio.ComputeFinalVolume(1.0f, "SFX") * listenerGain;
    EXPECT_FLOAT_EQ(effective, 0.5f);
}

TEST_F(AudioSystemTest, MasterVolumeSurvivesReinitialize)
{
    auto &audio = AudioSystem::Instance();
    audio.SetMasterVolume(0.25f);

    audio.Shutdown();
    ASSERT_TRUE(audio.Initialize());

    ALfloat listenerGain = 0.0f;
    alGetListenerf(AL_GAIN, &listenerGain);
    EXPECT_FLOAT_EQ(audio.GetMasterVolume(), 0.25f);
    EXPECT_FLOAT_EQ(listenerGain, 0.25f);
}

TEST_F(AudioSystemTest, PlayOneShotRejectsMissingOrUnloadedClip)
{
    auto &audio = AudioSystem::Instance();

    EXPECT_FALSE(audio.PlayOneShot(nullptr).IsValid());
    EXPECT_FALSE(audio.PlayOneShot(std::make_shared<AudioClip>()).IsValid());
}

TEST_F(AudioSystemTest, PlayOneShotStartsPlayback)
{
    auto &audio = AudioSystem::Instance();

    AudioHandle handle = PlayOneShot(_longClip);

    ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(audio.IsPlaying(handle));
    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 1u);
}

TEST_F(AudioSystemTest, PlayOneShotReturnsUniqueHandles)
{
    auto &audio = AudioSystem::Instance();

    AudioHandle a = PlayOneShot(_longClip);
    AudioHandle b = PlayOneShot(_longClip);

    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    EXPECT_NE(a, b);
}

TEST_F(AudioSystemTest, StopEndsPlayback)
{
    auto &audio = AudioSystem::Instance();
    AudioHandle handle = PlayOneShot(_longClip);
    ASSERT_TRUE(audio.IsPlaying(handle));

    audio.Stop(handle);

    EXPECT_FALSE(audio.IsPlaying(handle));
    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 0u);
}

TEST_F(AudioSystemTest, StopAndIsPlayingIgnoreUnknownHandles)
{
    auto &audio = AudioSystem::Instance();

    audio.Stop(AudioHandle{});
    audio.Stop(AudioHandle{9999});

    EXPECT_FALSE(audio.IsPlaying(AudioHandle{}));
    EXPECT_FALSE(audio.IsPlaying(AudioHandle{9999}));
}

TEST_F(AudioSystemTest, FinishedOneShotIsCleanedUpByUpdate)
{
    auto &audio = AudioSystem::Instance();
    AudioHandle handle = PlayOneShot(_shortClip);
    ASSERT_TRUE(handle.IsValid());

    ASSERT_TRUE(WaitUntil([&] { return !audio.IsPlaying(handle); }))
        << "50ms clip never finished; is the OpenAL null backend running?";

    audio.Update();

    // After cleanup the handle is no longer tracked at all
    EXPECT_FALSE(audio.IsPlaying(handle));
    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 0u);
}

TEST_F(AudioSystemTest, LoopingOneShotKeepsPlaying)
{
    auto &audio = AudioSystem::Instance();
    AudioHandle handle = PlayOneShot(_shortClip, {.loop = true});
    ASSERT_TRUE(handle.IsValid());

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // several loops of a 50ms clip
    audio.Update();

    EXPECT_TRUE(audio.IsPlaying(handle));
    audio.Stop(handle);
}

TEST_F(AudioSystemTest, MaxConcurrentLimitsOneShots)
{
    auto &audio = AudioSystem::Instance();
    const PlaybackParams music{.loop = true, .mixerGroup = "Music"}; // Music allows 2

    AudioHandle first = PlayOneShot(_longClip, music);
    AudioHandle second = PlayOneShot(_longClip, music);
    AudioHandle third = PlayOneShot(_longClip, music);

    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(second.IsValid());
    EXPECT_FALSE(third.IsValid());
    EXPECT_EQ(audio.CountPlayingInGroup("Music"), 2u);

    // Other groups are unaffected
    EXPECT_TRUE(PlayOneShot(_longClip).IsValid());

    // Stopping one frees a slot
    audio.Stop(first);
    EXPECT_TRUE(PlayOneShot(_longClip, music).IsValid());
}

TEST_F(AudioSystemTest, SetGroupMaxConcurrentAppliesToNewPlayback)
{
    auto &audio = AudioSystem::Instance();
    audio.SetGroupMaxConcurrent("SFX", 1);

    EXPECT_TRUE(PlayOneShot(_longClip).IsValid());
    EXPECT_FALSE(PlayOneShot(_longClip).IsValid());
}

TEST_F(AudioSystemTest, ReleasedSourcesAreReset)
{
    auto &audio = AudioSystem::Instance();
    ALuint source = audio.AcquireSource();
    ASSERT_NE(source, 0u);

    alSourcef(source, AL_GAIN, 0.3f);
    alSourcef(source, AL_PITCH, 1.7f);
    alSourcei(source, AL_LOOPING, AL_TRUE);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(_longClip->GetBuffer()));
    audio.ReleaseSource(source);

    ALuint reused = audio.AcquireSource(); // pool is LIFO
    ASSERT_EQ(reused, source);

    ALfloat gain = 0.0f, pitch = 0.0f;
    ALint looping = AL_TRUE, buffer = -1;
    alGetSourcef(reused, AL_GAIN, &gain);
    alGetSourcef(reused, AL_PITCH, &pitch);
    alGetSourcei(reused, AL_LOOPING, &looping);
    alGetSourcei(reused, AL_BUFFER, &buffer);

    EXPECT_FLOAT_EQ(gain, 1.0f);
    EXPECT_FLOAT_EQ(pitch, 1.0f);
    EXPECT_EQ(looping, AL_FALSE);
    EXPECT_EQ(buffer, 0);

    audio.ReleaseSource(reused);
}

TEST_F(AudioSystemTest, AcquireSourceGrowsBeyondPool)
{
    auto &audio = AudioSystem::Instance();
    std::vector<ALuint> sources;

    for (int i = 0; i < 40; ++i) // pool holds 32
    {
        ALuint source = audio.AcquireSource();
        ASSERT_NE(source, 0u) << "acquire " << i;
        sources.push_back(source);
    }

    for (ALuint source : sources)
    {
        audio.ReleaseSource(source);
    }
}

// ============================================================================
// AudioSource component
// ============================================================================

class AudioSourceTest : public AudioContextTest
{
protected:
    GameObject::Ptr _go;
    AudioSource *_source = nullptr;
    std::shared_ptr<AudioClip> _clip;

    void SetUp() override
    {
        AudioContextTest::SetUp();
        _clip = MakeClip(5.0f);
        ASSERT_NE(_clip, nullptr);

        _go = GameObject::Create("AudioSourceTest");
        _source = _go->AddComponent<AudioSource>();
        ASSERT_NE(_source, nullptr);
    }

    void ReleaseResources() override
    {
        _source = nullptr;
        _go.reset(); // releases the AL source back to the system
        _clip.reset();
    }
};

TEST_F(AudioSourceTest, HasExpectedDefaults)
{
    EXPECT_EQ(_source->GetTypeName(), "AudioSource");
    EXPECT_EQ(_source->GetClip(), nullptr);
    EXPECT_FLOAT_EQ(_source->GetVolume(), 1.0f);
    EXPECT_FLOAT_EQ(_source->GetPitch(), 1.0f);
    EXPECT_FALSE(_source->GetLoop());
    EXPECT_TRUE(_source->GetSpatial());
    EXPECT_EQ(_source->GetMixerGroup(), "SFX");
    EXPECT_FALSE(_source->IsPlaying());
    EXPECT_FALSE(_source->IsPaused());
}

TEST_F(AudioSourceTest, SettersClampValues)
{
    _source->SetVolume(2.0f);
    EXPECT_FLOAT_EQ(_source->GetVolume(), 1.0f);
    _source->SetVolume(-1.0f);
    EXPECT_FLOAT_EQ(_source->GetVolume(), 0.0f);

    _source->SetPitch(5.0f);
    EXPECT_FLOAT_EQ(_source->GetPitch(), 2.0f);
    _source->SetPitch(0.0f);
    EXPECT_FLOAT_EQ(_source->GetPitch(), 0.5f);
}

TEST_F(AudioSourceTest, PlayWithoutClipDoesNothing)
{
    _source->Play();

    EXPECT_FALSE(_source->IsPlaying());
}

TEST_F(AudioSourceTest, PlayPauseStop)
{
    _source->SetSpatial(false);
    _source->SetClip(_clip);

    _source->Play();
    EXPECT_TRUE(_source->IsPlaying());
    EXPECT_FALSE(_source->IsPaused());

    _source->Pause();
    EXPECT_FALSE(_source->IsPlaying());
    EXPECT_TRUE(_source->IsPaused());

    _source->Play();
    EXPECT_TRUE(_source->IsPlaying());

    _source->Stop();
    EXPECT_FALSE(_source->IsPlaying());
    EXPECT_FALSE(_source->IsPaused());
}

TEST_F(AudioSourceTest, SpatialPlaybackCreatesPositionable)
{
    ASSERT_TRUE(_source->GetSpatial());
    _source->SetClip(_clip);

    _source->Play();

    EXPECT_TRUE(_source->IsPlaying());
    EXPECT_TRUE(_go->HasPositionable());
}

TEST_F(AudioSourceTest, SetClipWhilePlayingKeepsPlaying)
{
    _source->SetSpatial(false);
    _source->SetClip(_clip);
    _source->Play();
    ASSERT_TRUE(_source->IsPlaying());

    auto other = MakeClip(3.0f);
    ASSERT_NE(other, nullptr);
    _source->SetClip(other);

    EXPECT_EQ(_source->GetClip(), other);
    EXPECT_TRUE(_source->IsPlaying());

    _source->Stop();
}

TEST_F(AudioSourceTest, SetClipWhileStoppedDoesNotStartPlayback)
{
    _source->SetClip(_clip);

    EXPECT_EQ(_source->GetClip(), _clip);
    EXPECT_FALSE(_source->IsPlaying());
}

TEST_F(AudioSourceTest, RespectsGroupConcurrencyLimitFromOneShots)
{
    auto &audio = AudioSystem::Instance();
    audio.SetGroupMaxConcurrent("SFX", 1);
    ASSERT_TRUE(PlayOneShot(_clip, {.loop = true}).IsValid());

    _source->SetSpatial(false);
    _source->SetClip(_clip);
    _source->Play();

    EXPECT_FALSE(_source->IsPlaying());
}

// Known gap: CountPlayingInGroup only tracks one-shots, so playing AudioSource
// components never count toward a group's maxConcurrent limit.
TEST_F(AudioSourceTest, DISABLED_PlayingSourcesCountTowardGroupLimit)
{
    auto &audio = AudioSystem::Instance();
    audio.SetGroupMaxConcurrent("SFX", 1);

    _source->SetSpatial(false);
    _source->SetClip(_clip);
    _source->Play();
    ASSERT_TRUE(_source->IsPlaying());

    EXPECT_EQ(audio.CountPlayingInGroup("SFX"), 1u);
    EXPECT_FALSE(PlayOneShot(_clip).IsValid());
}
