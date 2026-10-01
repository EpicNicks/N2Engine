#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/Logger.hpp"

#include <limits>
#include <ranges>

namespace N2Engine::Audio
{
    AudioSystem& AudioSystem::Instance()
    {
        // Never destroyed: AudioSources unregister in their destructors, which can run during static
        // destruction (SceneManager's loaded scene). When AudioSystem was first used after SceneManager
        // was created (headless, where Init skips audio), a function-local static died first.
        static AudioSystem *instance = new AudioSystem();
        return *instance;
    }

    bool AudioSystem::Initialize()
    {
        if (_initialized)
        {
            return true;
        }

        _device = alcOpenDevice(nullptr);
        if (!_device)
        {
            Logger::Error("Failed to open OpenAL device");
            return false;
        }

        _context = alcCreateContext(_device, nullptr);
        if (!_context)
        {
            Logger::Error("Failed to create OpenAL context");
            alcCloseDevice(_device);
            _device = nullptr;
            return false;
        }

        if (!alcMakeContextCurrent(_context))
        {
            Logger::Error("Failed to make OpenAL context current");
            alcDestroyContext(_context);
            alcCloseDevice(_device);
            _context = nullptr;
            _device = nullptr;
            return false;
        }

        // Pre-allocate source pool
        _sourcePool.reserve(MaxPooledSources);
        for (std::size_t i = 0; i < MaxPooledSources; ++i)
        {
            ALuint source;
            alGenSources(1, &source);
            if (alGetError() == AL_NO_ERROR)
            {
                _sourcePool.push_back(source);
            }
        }

        InitializeDefaultGroups();

        // A new context starts with listener gain 1; restore the master volume
        alListenerf(AL_GAIN, _masterVolume);

        _initialized = true;
        ++_contextGeneration;
        Logger::Info(std::format("AudioSystem initialized with {} pooled sources", _sourcePool.size()));

        return true;
    }

    void AudioSystem::Shutdown()
    {
        if (!_initialized)
        {
            return;
        }

        // Stop and delete all one-shot sources
        for (auto& [handle, oneShot] : _oneShots)
        {
            alSourceStop(oneShot.source);
            alDeleteSources(1, &oneShot.source);
        }
        _oneShots.clear();

        // Delete pooled sources
        for (ALuint source : _sourcePool)
        {
            alDeleteSources(1, &source);
        }
        _sourcePool.clear();

        // Sources held by components die with the context; drop their ids, so after a later Initialize
        // they don't play through an id that now belongs to someone else
        for (AudioSource *source : _sources)
        {
            if (const ALuint held = source->GetSourceHandle(); held != 0)
            {
                alSourceStop(held);
                alDeleteSources(1, &held);
                source->ForgetSource();
            }
        }

        alcMakeContextCurrent(nullptr);

        if (_context)
        {
            alcDestroyContext(_context);
            _context = nullptr;
        }

        if (_device)
        {
            alcCloseDevice(_device);
            _device = nullptr;
        }

        _initialized = false;
        Logger::Info("AudioSystem shutdown");
    }

    void AudioSystem::Update()
    {
        if (!_initialized)
        {
            return;
        }

        CleanupFinishedSources();
    }

    void AudioSystem::InitializeDefaultGroups()
    {
        CreateMixerGroup("Master");
        CreateMixerGroup("Music", { .maxConcurrent = 2 });
        CreateMixerGroup("SFX");
        CreateMixerGroup("UI", { .maxSourceVolume = 0.8f });
        CreateMixerGroup("Voice", { .maxConcurrent = 4 });
        CreateMixerGroup("Ambient");
    }

    void AudioSystem::CreateMixerGroup(const std::string& name, const AudioMixerGroupSettings& settings)
    {
        _mixerGroups[name] = AudioMixerGroup{
            .name = name,
            .settings = settings
        };
    }

    void AudioSystem::SetGroupVolume(const std::string& group, float volume)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.volume = std::clamp(volume, 0.0f, 1.0f);
            RefreshGroup(group);
        }
    }

    void AudioSystem::SetGroupMaxVolume(const std::string& group, float maxVolume)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.maxSourceVolume = std::clamp(maxVolume, 0.0f, 1.0f);
            RefreshGroup(group);
        }
    }

    void AudioSystem::SetGroupPitch(const std::string& group, float pitch)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.pitch = std::clamp(pitch, 0.5f, 2.0f);
            RefreshGroup(group);
        }
    }

    void AudioSystem::SetGroupMuted(const std::string& group, bool muted)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.muted = muted;
            RefreshGroup(group);
        }
    }

    void AudioSystem::SetGroupMaxConcurrent(const std::string& group, std::uint32_t max)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.maxConcurrent = max;
        }
    }

    AudioMixerGroup* AudioSystem::GetMixerGroup(const std::string& name)
    {
        auto it = _mixerGroups.find(name);
        return it != _mixerGroups.end() ? &it->second : nullptr;
    }

    const AudioMixerGroup* AudioSystem::GetMixerGroup(const std::string& name) const
    {
        auto it = _mixerGroups.find(name);
        return it != _mixerGroups.end() ? &it->second : nullptr;
    }

    void AudioSystem::SetMasterVolume(float volume)
    {
        _masterVolume = std::clamp(volume, 0.0f, 1.0f);
        // Without a context there's no listener; Initialize applies the stored value
        if (_initialized)
        {
            alListenerf(AL_GAIN, _masterVolume);
        }
    }

    float AudioSystem::ComputeFinalVolume(float sourceVolume, const std::string& group) const
    {
        float final = sourceVolume;

        if (const auto* g = GetMixerGroup(group))
        {
            if (g->settings.muted)
            {
                return 0.0f;
            }

            final = std::min(final, g->settings.maxSourceVolume);
            final *= g->settings.volume;
        }

        // Master volume is applied once, via the listener gain (see SetMasterVolume)
        return final;
    }

    AudioHandle AudioSystem::PlayOneShot(const std::shared_ptr<AudioClip>& clip, const PlaybackParams& params)
    {
        if (!_initialized || !clip || !clip->IsLoaded())
        {
            return AudioHandle{};
        }

        // Check concurrent limit
        if (const auto* group = GetMixerGroup(params.mixerGroup))
        {
            if (group->settings.maxConcurrent.has_value())
            {
                if (CountPlayingInGroup(params.mixerGroup) >= *group->settings.maxConcurrent)
                {
                    return AudioHandle{};
                }
            }
        }

        ALuint source = AcquireSource();
        if (source == 0)
        {
            Logger::Warn("No available audio sources for PlayOneShot");
            return AudioHandle{};
        }

        alSourcei(source, AL_BUFFER, static_cast<ALint>(clip->GetBuffer()));
        alSourcef(source, AL_GAIN, ComputeFinalVolume(params.volume, params.mixerGroup));
        alSourcef(source, AL_PITCH, ComputeFinalPitch(params.pitch, params.mixerGroup));
        alSourcei(source, AL_LOOPING, params.loop ? AL_TRUE : AL_FALSE);

        if (params.spatial)
        {
            alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
            alSource3f(source, AL_POSITION, params.x, params.y, params.z);
        }
        else
        {
            alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
            alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
            alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        }

        alSourcePlay(source);

        AudioHandle handle{ _nextHandleId++ };
        _oneShots[handle] = OneShot{
            .source = source,
            .group = params.mixerGroup,
            .volume = params.volume,
            .pitch = params.pitch,
            .clip = clip,
        };

        return handle;
    }

    void AudioSystem::Stop(AudioHandle handle)
    {
        auto it = _oneShots.find(handle);
        if (it != _oneShots.end())
        {
            alSourceStop(it->second.source);
            ReleaseSource(it->second.source);
            _oneShots.erase(it);
        }
    }

    ALuint AudioSystem::GetOneShotSource(AudioHandle handle) const
    {
        const auto it = _oneShots.find(handle);
        return it != _oneShots.end() ? it->second.source : 0;
    }

    bool AudioSystem::IsPlaying(AudioHandle handle) const
    {
        auto it = _oneShots.find(handle);
        if (it != _oneShots.end())
        {
            ALint state;
            alGetSourcei(it->second.source, AL_SOURCE_STATE, &state);
            return state == AL_PLAYING;
        }
        return false;
    }

    void AudioSystem::SetListenerPosition(float x, float y, float z)
    {
        // No context, no listener (an AudioListener pushes its pose again every LateUpdate)
        if (!_initialized)
        {
            return;
        }
        alListener3f(AL_POSITION, x, y, z);
    }

    void AudioSystem::SetListenerOrientation(float forwardX, float forwardY, float forwardZ,
                                              float upX, float upY, float upZ)
    {
        if (!_initialized)
        {
            return;
        }
        ALfloat orientation[] = { forwardX, forwardY, forwardZ, upX, upY, upZ };
        alListenerfv(AL_ORIENTATION, orientation);
    }

    ALuint AudioSystem::AcquireSource()
    {
        if (!_initialized)
        {
            return 0;
        }

        if (!_sourcePool.empty())
        {
            ALuint source = _sourcePool.back();
            _sourcePool.pop_back();
            return source;
        }

        // Pool empty, try to create a new source. Clear any stale error first, or an unrelated earlier
        // failure would make this look failed and leak the source it just created.
        alGetError();
        ALuint source;
        alGenSources(1, &source);
        if (alGetError() != AL_NO_ERROR)
        {
            return 0;
        }
        return source;
    }

    void AudioSystem::ReleaseSource(ALuint source)
    {
        if (source == 0)
        {
            return;
        }

        // After Shutdown the context is gone and the id means nothing; pooling it would hand a stale
        // id out again (aliasing a fresh source) after the next Initialize
        if (!_initialized)
        {
            return;
        }

        // Reset source state (everything AudioSource/PlayOneShot may have set)
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alSourcef(source, AL_GAIN, 1.0f);
        alSourcef(source, AL_PITCH, 1.0f);
        alSourcei(source, AL_LOOPING, AL_FALSE);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);
        alSourcef(source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(source, AL_MAX_DISTANCE, std::numeric_limits<float>::max());

        if (_sourcePool.size() < MaxPooledSources)
        {
            _sourcePool.push_back(source);
        }
        else
        {
            alDeleteSources(1, &source);
        }
    }

    std::uint32_t AudioSystem::CountPlayingInGroup(const std::string& group) const
    {
        // One-shots and AudioSource components both count (sources used to be ignored)
        std::uint32_t count = 0;
        for (const auto& [handle, oneShot] : _oneShots)
        {
            if (oneShot.group == group && IsPlaying(handle))
            {
                ++count;
            }
        }
        for (const AudioSource* source : _sources)
        {
            if (source->GetMixerGroup() == group && source->IsPlaying())
            {
                ++count;
            }
        }
        return count;
    }

    void AudioSystem::CleanupFinishedSources()
    {
        std::vector<AudioHandle> finished;

        for (const auto& [handle, oneShot] : _oneShots)
        {
            ALint state;
            alGetSourcei(oneShot.source, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED)
            {
                finished.push_back(handle);
            }
        }

        for (AudioHandle handle : finished)
        {
            ReleaseSource(_oneShots[handle].source);
            _oneShots.erase(handle); // releases the clip it kept alive
        }
    }

    void AudioSystem::RefreshGroup(const std::string& group)
    {
        if (!_initialized)
        {
            return;
        }
        for (const auto& oneShot : _oneShots | std::views::values)
        {
            if (oneShot.group == group)
            {
                alSourcef(oneShot.source, AL_GAIN, ComputeFinalVolume(oneShot.volume, group));
                alSourcef(oneShot.source, AL_PITCH, ComputeFinalPitch(oneShot.pitch, group));
            }
        }
        for (AudioSource* source : _sources)
        {
            if (source->GetMixerGroup() == group)
            {
                source->ApplyMixing();
            }
        }
    }

    float AudioSystem::ComputeFinalPitch(float sourcePitch, const std::string& group) const
    {
        // A group's pitch scales every sound in it (it used to be stored but never applied)
        const auto* g = GetMixerGroup(group);
        return sourcePitch * (g ? g->settings.pitch : 1.0f);
    }

    void AudioSystem::RegisterSource(AudioSource* source)
    {
        _sources.insert(source);
    }

    void AudioSystem::UnregisterSource(AudioSource* source)
    {
        _sources.erase(source);
    }
}