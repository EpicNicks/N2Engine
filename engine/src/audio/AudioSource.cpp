#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Audio
{
    AudioSource::AudioSource(GameObject& gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(_volume), _volume);
        RegisterMember(NAMEOF(_pitch), _pitch);
        RegisterMember(NAMEOF(_loop), _loop);
        RegisterMember(NAMEOF(_spatial), _spatial);
        RegisterMember(NAMEOF(_playOnAwake), _playOnAwake);
        RegisterMember(NAMEOF(_minDistance), _minDistance);
        RegisterMember(NAMEOF(_maxDistance), _maxDistance);
        RegisterMember(NAMEOF(_rolloffFactor), _rolloffFactor);
        RegisterMember(NAMEOF(_mixerGroup), _mixerGroup);
        RegisterAssetRef(NAMEOF(_clip), _clip);
    }

    AudioSource::~AudioSource()
    {
        if (_source != 0)
        {
            alSourceStop(_source);
            AudioSystem::Instance().ReleaseSource(_source);
            _source = 0;
        }
    }

    std::string AudioSource::GetTypeName() const
    {
        return NAMEOF(AudioSource);
    }

    void AudioSource::EnsureSource()
    {
        if (_source == 0)
        {
            _source = AudioSystem::Instance().AcquireSource();
        }
    }

    void AudioSource::Play()
    {
        if (!_clip || !_clip->IsLoaded())
        {
            return;
        }

        // Check concurrent limit
        auto& audio = AudioSystem::Instance();
        if (auto* group = audio.GetMixerGroup(_mixerGroup))
        {
            if (group->settings.maxConcurrent.has_value())
            {
                if (audio.CountPlayingInGroup(_mixerGroup) >= *group->settings.maxConcurrent)
                {
                    return;
                }
            }
        }

        EnsureSource();
        if (_source == 0)
        {
            return;
        }

        alSourcei(_source, AL_BUFFER, static_cast<ALint>(_clip->GetBuffer()));
        UpdateVolume();
        alSourcef(_source, AL_PITCH, _pitch);
        alSourcei(_source, AL_LOOPING, _loop ? AL_TRUE : AL_FALSE);

        if (_spatial)
        {
            alSourcei(_source, AL_SOURCE_RELATIVE, AL_FALSE);
            alSourcef(_source, AL_REFERENCE_DISTANCE, _minDistance);
            alSourcef(_source, AL_MAX_DISTANCE, _maxDistance);
            alSourcef(_source, AL_ROLLOFF_FACTOR, _rolloffFactor);
            UpdatePosition();
        }
        else
        {
            alSourcei(_source, AL_SOURCE_RELATIVE, AL_TRUE);
            alSource3f(_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
            alSourcef(_source, AL_ROLLOFF_FACTOR, 0.0f);
        }

        alSourcePlay(_source);
    }

    void AudioSource::Pause()
    {
        if (_source != 0)
        {
            alSourcePause(_source);
        }
    }

    void AudioSource::Stop()
    {
        if (_source != 0)
        {
            alSourceStop(_source);
        }
    }

    bool AudioSource::IsPlaying() const
    {
        if (_source == 0)
        {
            return false;
        }

        ALint state;
        alGetSourcei(_source, AL_SOURCE_STATE, &state);
        return state == AL_PLAYING;
    }

    bool AudioSource::IsPaused() const
    {
        if (_source == 0)
        {
            return false;
        }

        ALint state;
        alGetSourcei(_source, AL_SOURCE_STATE, &state);
        return state == AL_PAUSED;
    }

    void AudioSource::SetClip(std::shared_ptr<AudioClip> clip)
    {
        bool wasPlaying = IsPlaying();
        Stop();

        _clip = std::move(clip);

        if (wasPlaying && _clip)
        {
            Play();
        }
    }

    void AudioSource::SetVolume(float volume)
    {
        _volume = std::clamp(volume, 0.0f, 1.0f);
        UpdateVolume();
    }

    void AudioSource::SetPitch(float pitch)
    {
        _pitch = std::clamp(pitch, 0.5f, 2.0f);
        if (_source != 0)
        {
            alSourcef(_source, AL_PITCH, _pitch);
        }
    }

    void AudioSource::SetLoop(bool loop)
    {
        _loop = loop;
        if (_source != 0)
        {
            alSourcei(_source, AL_LOOPING, _loop ? AL_TRUE : AL_FALSE);
        }
    }

    void AudioSource::SetSpatial(bool spatial)
    {
        _spatial = spatial;
        if (_source != 0)
        {
            if (_spatial)
            {
                alSourcei(_source, AL_SOURCE_RELATIVE, AL_FALSE);
                alSourcef(_source, AL_ROLLOFF_FACTOR, _rolloffFactor);
                UpdatePosition();
            }
            else
            {
                alSourcei(_source, AL_SOURCE_RELATIVE, AL_TRUE);
                alSource3f(_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
                alSourcef(_source, AL_ROLLOFF_FACTOR, 0.0f);
            }
        }
    }

    void AudioSource::SetMixerGroup(const std::string& group)
    {
        _mixerGroup = group;
        UpdateVolume();
    }

    void AudioSource::SetMinDistance(float distance)
    {
        _minDistance = std::max(0.0f, distance);
        if (_source != 0 && _spatial)
        {
            alSourcef(_source, AL_REFERENCE_DISTANCE, _minDistance);
        }
    }

    void AudioSource::SetMaxDistance(float distance)
    {
        _maxDistance = std::max(_minDistance, distance);
        if (_source != 0 && _spatial)
        {
            alSourcef(_source, AL_MAX_DISTANCE, _maxDistance);
        }
    }

    void AudioSource::SetRolloffFactor(float factor)
    {
        _rolloffFactor = std::max(0.0f, factor);
        if (_source != 0 && _spatial)
        {
            alSourcef(_source, AL_ROLLOFF_FACTOR, _rolloffFactor);
        }
    }

    void AudioSource::UpdateVolume()
    {
        if (_source != 0)
        {
            float finalVolume = AudioSystem::Instance().ComputeFinalVolume(_volume, _mixerGroup);
            alSourcef(_source, AL_GAIN, finalVolume);
        }
    }

    void AudioSource::UpdatePosition()
    {
        if (_source != 0 && _spatial)
        {
            if (!GetGameObject().HasPositionable())
            {
                GetGameObject().CreatePositionable();
            }
            Positionable *transform = GetGameObject().GetPositionable();

            // Use global transform for world-space position
            auto pos = transform->GetGlobalTransform().GetPosition();
            alSource3f(_source, AL_POSITION, pos.x, pos.y, pos.z);
        }
    }

    void AudioSource::OnEnable()
    {
        if (_playOnAwake && _clip)
        {
            Play();
        }
    }

    void AudioSource::OnDisable()
    {
        Pause();
    }

    void AudioSource::OnDestroy()
    {
        if (_source != 0)
        {
            alSourceStop(_source);
            AudioSystem::Instance().ReleaseSource(_source);
            _source = 0;
        }
    }

    void AudioSource::OnLateUpdate()
    {
        if (_spatial && IsPlaying())
        {
            UpdatePosition();
        }
    }
}