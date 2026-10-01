#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include <AL/al.h>
#include <AL/alc.h>

#include "AudioHandle.hpp"
#include "AudioMixerGroup.hpp"

namespace N2Engine::Audio
{
    class AudioClip;
    class AudioSource;

    struct PlaybackParams
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        bool spatial = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool loop = false;
        std::string mixerGroup = "SFX";
    };

    class AudioSystem
    {
    public:
        static AudioSystem& Instance();

        bool Initialize();
        void Shutdown();
        [[nodiscard]] bool IsInitialized() const { return _initialized; }
        /// Changes with every successful Initialize. OpenAL objects made under an earlier context (an
        /// AudioClip's buffer) died with it, so their ids must not be deleted in the new one.
        [[nodiscard]] std::uint32_t GetContextGeneration() const { return _contextGeneration; }
        void Update();

        // Mixer group management
        void CreateMixerGroup(const std::string& name, const AudioMixerGroupSettings& settings = {});
        void SetGroupVolume(const std::string& group, float volume);
        void SetGroupMaxVolume(const std::string& group, float maxVolume);
        void SetGroupPitch(const std::string& group, float pitch);
        void SetGroupMuted(const std::string& group, bool muted);
        void SetGroupMaxConcurrent(const std::string& group, std::uint32_t max);

        AudioMixerGroup* GetMixerGroup(const std::string& name);
        const AudioMixerGroup* GetMixerGroup(const std::string& name) const;

        // Master controls
        void SetMasterVolume(float volume);
        [[nodiscard]] float GetMasterVolume() const { return _masterVolume; }

        // One-shot playback (fire and forget)
        AudioHandle PlayOneShot(const std::shared_ptr<AudioClip>& clip, const PlaybackParams& params = {});
        void Stop(AudioHandle handle);
        [[nodiscard]] bool IsPlaying(AudioHandle handle) const;
        /// The OpenAL source playing a one-shot, or 0 if the handle is unknown
        [[nodiscard]] ALuint GetOneShotSource(AudioHandle handle) const;

        // Listener
        void SetListenerPosition(float x, float y, float z);
        void SetListenerOrientation(float forwardX, float forwardY, float forwardZ,
                                     float upX, float upY, float upZ);

        // Volume/pitch computation (master volume is the listener gain, applied separately)
        [[nodiscard]] float ComputeFinalVolume(float sourceVolume, const std::string& group) const;
        [[nodiscard]] float ComputeFinalPitch(float sourcePitch, const std::string& group) const;

        /// AudioSources register themselves, so group limits count them and group changes
        /// (volume, mute, pitch) reach them while they play
        void RegisterSource(AudioSource* source);
        void UnregisterSource(AudioSource* source);

        // For AudioSource to register/unregister
        [[nodiscard]] ALuint AcquireSource();
        void ReleaseSource(ALuint source);

        [[nodiscard]] std::uint32_t CountPlayingInGroup(const std::string& group) const;

    private:
        AudioSystem() = default;
        ~AudioSystem() = default;

        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        void InitializeDefaultGroups();
        void CleanupFinishedSources();
        /// Re-applies a group's current settings to everything playing in it
        void RefreshGroup(const std::string& group);

        ALCdevice* _device = nullptr;
        ALCcontext* _context = nullptr;

        std::unordered_map<std::string, AudioMixerGroup> _mixerGroups;
        struct OneShot
        {
            ALuint source = 0;
            std::string group;
            float volume = 1.0f;
            float pitch = 1.0f;
            std::shared_ptr<AudioClip> clip; // keeps the buffer alive until playback ends
        };
        std::unordered_map<AudioHandle, OneShot> _oneShots;
        std::unordered_set<AudioSource*> _sources;

        std::vector<ALuint> _sourcePool;
        static constexpr std::size_t MaxPooledSources = 32;

        std::uint32_t _nextHandleId = 1;
        float _masterVolume = 1.0f;
        bool _initialized = false;
        std::uint32_t _contextGeneration = 0;
    };
}