#pragma once

#include <string>
#include <unordered_map>
#include <memory>

#include <AL/al.h>
#include <AL/alc.h>

#include "AudioHandle.hpp"
#include "AudioMixerGroup.hpp"

namespace N2Engine::Audio
{
    class AudioClip;

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

        // Listener
        void SetListenerPosition(float x, float y, float z);
        void SetListenerOrientation(float forwardX, float forwardY, float forwardZ,
                                     float upX, float upY, float upZ);

        // Volume computation
        [[nodiscard]] float ComputeFinalVolume(float sourceVolume, const std::string& group) const;

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

        ALCdevice* _device = nullptr;
        ALCcontext* _context = nullptr;

        std::unordered_map<std::string, AudioMixerGroup> _mixerGroups;
        std::unordered_map<AudioHandle, ALuint> _activeOneShotSources;
        std::unordered_map<AudioHandle, std::string> _oneShotGroups;

        std::vector<ALuint> _sourcePool;
        static constexpr std::size_t MaxPooledSources = 32;

        std::uint32_t _nextHandleId = 1;
        float _masterVolume = 1.0f;
        bool _initialized = false;
    };
}