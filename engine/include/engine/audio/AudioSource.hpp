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

        /// Whether it starts playing its clip when enabled
        void SetPlayOnAwake(bool playOnAwake) { _playOnAwake = playOnAwake; }
        [[nodiscard]] bool GetPlayOnAwake() const { return _playOnAwake; }

        /// Re-applies volume and pitch with the mixer group's current settings (AudioSystem calls this
        /// when a group changes, so it reaches sounds that are already playing)
        void ApplyMixing();

        /// The OpenAL source id while it has one (0 otherwise); for diagnostics and tests
        [[nodiscard]] ALuint GetSourceHandle() const { return _source; }
        /// AudioSystem calls this on Shutdown: the OpenAL context is going away, so the id is meaningless
        void ForgetSource() { _source = 0; }

        // 3D audio settings
        void SetMinDistance(float distance);
        void SetMaxDistance(float distance);
        void SetRolloffFactor(float factor);
        [[nodiscard]] float GetMinDistance() const { return _minDistance; }
        [[nodiscard]] float GetMaxDistance() const { return _maxDistance; }
        [[nodiscard]] float GetRolloffFactor() const { return _rolloffFactor; }

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