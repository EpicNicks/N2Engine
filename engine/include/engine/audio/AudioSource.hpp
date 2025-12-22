#pragma once

#include <string>
#include <memory>

#include <AL/al.h>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Audio
{
    class AudioClip;

    class AudioSource : public SerializableComponent
    {
    public:
        explicit AudioSource(GameObject& gameObject);
        ~AudioSource() override;

        [[nodiscard]] std::string GetTypeName() const override;

        // Playback
        void Play();
        void Pause();
        void Stop();
        [[nodiscard]] bool IsPlaying() const;
        [[nodiscard]] bool IsPaused() const;

        // Properties
        void SetClip(std::shared_ptr<AudioClip> clip);
        [[nodiscard]] std::shared_ptr<AudioClip> GetClip() const { return _clip; }

        void SetVolume(float volume);
        [[nodiscard]] float GetVolume() const { return _volume; }

        void SetPitch(float pitch);
        [[nodiscard]] float GetPitch() const { return _pitch; }

        void SetLoop(bool loop);
        [[nodiscard]] bool GetLoop() const { return _loop; }

        void SetSpatial(bool spatial);
        [[nodiscard]] bool GetSpatial() const { return _spatial; }

        void SetMixerGroup(const std::string& group);
        [[nodiscard]] const std::string& GetMixerGroup() const { return _mixerGroup; }

        // 3D audio settings
        void SetMinDistance(float distance);
        void SetMaxDistance(float distance);
        void SetRolloffFactor(float factor);

        // Lifecycle
        void OnEnable() override;
        void OnDisable() override;
        void OnDestroy() override;
        void OnLateUpdate() override;

        static constexpr bool IsSingleton = false;

    private:
        void EnsureSource();
        void UpdateVolume();
        void UpdatePosition();

        ALuint _source = 0;
        std::shared_ptr<AudioClip> _clip;

        float _volume = 1.0f;
        float _pitch = 1.0f;
        bool _loop = false;
        bool _spatial = true;
        bool _playOnAwake = false;

        float _minDistance = 1.0f;
        float _maxDistance = 100.0f;
        float _rolloffFactor = 1.0f;

        std::string _mixerGroup = "SFX";
    };
}