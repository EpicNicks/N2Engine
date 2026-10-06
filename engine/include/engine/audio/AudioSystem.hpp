#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h> // ALC_SOFT_loopback

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

    /// Where the mixed audio goes
    enum class AudioOutput
    {
        /// The default sound card, played locally (a normal game)
        Device,
        /// An OpenAL Soft loopback device (ALC_SOFT_loopback): nothing plays locally, and the mix is pulled with
        /// RenderSamples / AdvanceStream, e.g. to stream it from a headless engine to the editor client
        Loopback,
    };

    enum class SampleFormat
    {
        Int16,   // signed 16-bit, little-endian
        Float32, // IEEE float, little-endian, nominally -1..1
    };

    /// "int16" or "float32", as the editor protocol's AudioSamples.sampleFormat names them
    [[nodiscard]] constexpr std::string_view ToString(SampleFormat format)
    {
        return format == SampleFormat::Float32 ? "float32" : "int16";
    }

    /// The loopback device's fixed output format; samples are interleaved (left, right, left, ...)
    struct LoopbackFormat
    {
        std::uint32_t sampleRate = 48000;
        std::uint32_t channels = 2;
        /// Float32 when OpenAL Soft supports it for this rate and layout (it does), else Int16
        SampleFormat sampleFormat = SampleFormat::Float32;

        [[nodiscard]] std::size_t BytesPerSample() const { return sampleFormat == SampleFormat::Float32 ? 4u : 2u; }
        [[nodiscard]] std::size_t BytesPerFrame() const { return BytesPerSample() * channels; }
    };

    /// Mixed audio taken from the loopback stream (see AudioSystem::TakeStreamedAudio)
    struct StreamedAudio
    {
        /// frameCount * LoopbackFormat::BytesPerFrame() bytes of interleaved samples, oldest first
        std::vector<std::uint8_t> samples;
        std::uint32_t frameCount = 0;
        /// Frames discarded since the previous take because the stream buffer was full (nobody drained it)
        std::uint32_t droppedFrames = 0;
    };

    class AudioSystem
    {
    public:
        static AudioSystem& Instance();

        /// Loopback rate and layout. 48 kHz stereo: the usual rate for game audio and for browser/desktop playback.
        static constexpr std::uint32_t LoopbackSampleRate = 48000;
        static constexpr std::uint32_t LoopbackChannels = 2;
        /// Most mixed audio the stream buffer keeps for the client; older frames are discarded beyond this
        static constexpr std::uint32_t StreamBufferMilliseconds = 250;
        /// Longest step AdvanceStream takes at once (a debugger pause shouldn't mix minutes of audio)
        static constexpr double MaxStreamAdvanceSeconds = 1.0;

        /// Opens the output (the default sound card, or a loopback device) and an OpenAL context on it.
        /// Idempotent for the same output; false if already initialized with the other one (Shutdown first).
        bool Initialize(AudioOutput output = AudioOutput::Device);
        void Shutdown();
        [[nodiscard]] bool IsInitialized() const { return _initialized; }
        /// Changes with every successful Initialize. OpenAL objects made under an earlier context (an
        /// AudioClip's buffer) died with it, so their ids must not be deleted in the new one.
        [[nodiscard]] std::uint32_t GetContextGeneration() const { return _contextGeneration; }
        void Update();

        // ==================== Loopback (headless streaming) ====================
        // Main thread only, like the rest of AudioSystem. A loopback device's clock only moves when samples are
        // rendered: until then nothing plays, one-shots never finish and their sources never return to the pool.

        /// Whether OpenAL Soft offers ALC_SOFT_loopback (no device or context needed to ask)
        [[nodiscard]] static bool IsLoopbackSupported();
        [[nodiscard]] AudioOutput GetOutput() const { return _output; }
        /// Initialized with AudioOutput::Loopback
        [[nodiscard]] bool IsLoopback() const { return _initialized && _output == AudioOutput::Loopback; }
        /// The loopback output format (meaningful while IsLoopback())
        [[nodiscard]] const LoopbackFormat& GetLoopbackFormat() const { return _loopbackFormat; }

        /// Mixes the next frameCount frames into buffer (frameCount * GetLoopbackFormat().BytesPerFrame() bytes),
        /// advancing the audio clock by that much. Bypasses the stream buffer. False (buffer untouched) unless
        /// IsLoopback().
        bool RenderSamples(void *buffer, std::uint32_t frameCount);

        /// Paces the loopback mix by elapsed time: adds unscaledSeconds * sampleRate frames to an accumulator
        /// (at most MaxStreamAdvanceSeconds per call) and renders the whole frames into the stream buffer. Call
        /// it every frame whether or not anyone reads the stream, so time advances and one-shots finish. No-op
        /// unless IsLoopback().
        void AdvanceStream(double unscaledSeconds);
        /// Moves everything in the stream buffer out (oldest first) and empties it
        [[nodiscard]] StreamedAudio TakeStreamedAudio();
        /// Discards the stream buffer's contents (the audio clock is unaffected)
        void ClearStream();
        /// Frames currently held in the stream buffer
        [[nodiscard]] std::uint32_t GetStreamedFrameCount() const;
        /// The stream buffer's capacity in frames (StreamBufferMilliseconds of audio); 0 unless IsLoopback()
        [[nodiscard]] std::uint32_t GetStreamCapacityFrames() const;

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

        bool OpenDevice();
        bool OpenLoopbackDevice();
        /// Makes the new context current and sets up the source pool, groups and listener (either output)
        bool FinishInitialize();
        /// Renders frames (in chunks) into the stream buffer, discarding the oldest beyond its capacity
        void RenderIntoStream(std::uint64_t frames);
        void PushStream(const std::uint8_t *data, std::size_t bytes);
        void ResetLoopbackState();

        void InitializeDefaultGroups();
        void CleanupFinishedSources();
        /// Re-applies a group's current settings to everything playing in it
        void RefreshGroup(const std::string& group);

        ALCdevice* _device = nullptr;
        ALCcontext* _context = nullptr;
        AudioOutput _output = AudioOutput::Device;

        // Loopback state. alcRenderSamplesSOFT is an extension entry point, loaded with alcGetProcAddress.
        LPALCRENDERSAMPLESSOFT _renderSamples = nullptr;
        LoopbackFormat _loopbackFormat;
        double _streamAccumulator = 0.0;          // fractional frames not yet rendered
        std::vector<std::uint8_t> _streamRing;    // ring buffer, StreamBufferMilliseconds of frames
        std::size_t _streamStart = 0;             // byte offset of the oldest frame
        std::size_t _streamSize = 0;              // bytes held
        std::uint64_t _streamDroppedFrames = 0;   // since the last TakeStreamedAudio
        std::vector<std::uint8_t> _streamScratch; // one render chunk

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