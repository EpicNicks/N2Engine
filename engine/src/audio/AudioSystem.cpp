#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::Audio
{
    AudioSystem& AudioSystem::Instance()
    {
        static AudioSystem instance;
        return instance;
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

        _initialized = true;
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
        for (auto& [handle, source] : _activeOneShotSources)
        {
            alSourceStop(source);
            alDeleteSources(1, &source);
        }
        _activeOneShotSources.clear();
        _oneShotGroups.clear();

        // Delete pooled sources
        for (ALuint source : _sourcePool)
        {
            alDeleteSources(1, &source);
        }
        _sourcePool.clear();

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
        }
    }

    void AudioSystem::SetGroupMaxVolume(const std::string& group, float maxVolume)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.maxSourceVolume = std::clamp(maxVolume, 0.0f, 1.0f);
        }
    }

    void AudioSystem::SetGroupPitch(const std::string& group, float pitch)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.pitch = std::clamp(pitch, 0.5f, 2.0f);
        }
    }

    void AudioSystem::SetGroupMuted(const std::string& group, bool muted)
    {
        if (auto* g = GetMixerGroup(group))
        {
            g->settings.muted = muted;
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
        alListenerf(AL_GAIN, _masterVolume);
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

        return final * _masterVolume;
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
        alSourcef(source, AL_PITCH, params.pitch);
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
        _activeOneShotSources[handle] = source;
        _oneShotGroups[handle] = params.mixerGroup;

        return handle;
    }

    void AudioSystem::Stop(AudioHandle handle)
    {
        auto it = _activeOneShotSources.find(handle);
        if (it != _activeOneShotSources.end())
        {
            alSourceStop(it->second);
            ReleaseSource(it->second);
            _activeOneShotSources.erase(it);
            _oneShotGroups.erase(handle);
        }
    }

    bool AudioSystem::IsPlaying(AudioHandle handle) const
    {
        auto it = _activeOneShotSources.find(handle);
        if (it != _activeOneShotSources.end())
        {
            ALint state;
            alGetSourcei(it->second, AL_SOURCE_STATE, &state);
            return state == AL_PLAYING;
        }
        return false;
    }

    void AudioSystem::SetListenerPosition(float x, float y, float z)
    {
        alListener3f(AL_POSITION, x, y, z);
    }

    void AudioSystem::SetListenerOrientation(float forwardX, float forwardY, float forwardZ,
                                              float upX, float upY, float upZ)
    {
        ALfloat orientation[] = { forwardX, forwardY, forwardZ, upX, upY, upZ };
        alListenerfv(AL_ORIENTATION, orientation);
    }

    ALuint AudioSystem::AcquireSource()
    {
        if (!_sourcePool.empty())
        {
            ALuint source = _sourcePool.back();
            _sourcePool.pop_back();
            return source;
        }

        // Pool empty, try to create a new source
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

        // Reset source state
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alSourcef(source, AL_GAIN, 1.0f);
        alSourcef(source, AL_PITCH, 1.0f);
        alSourcei(source, AL_LOOPING, AL_FALSE);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);

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
        std::uint32_t count = 0;
        for (const auto& [handle, groupName] : _oneShotGroups)
        {
            if (groupName == group && IsPlaying(handle))
            {
                ++count;
            }
        }
        return count;
    }

    void AudioSystem::CleanupFinishedSources()
    {
        std::vector<AudioHandle> finished;

        for (const auto& [handle, source] : _activeOneShotSources)
        {
            ALint state;
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED)
            {
                finished.push_back(handle);
            }
        }

        for (AudioHandle handle : finished)
        {
            ReleaseSource(_activeOneShotSources[handle]);
            _activeOneShotSources.erase(handle);
            _oneShotGroups.erase(handle);
        }
    }
}