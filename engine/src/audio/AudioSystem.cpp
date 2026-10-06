#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>

namespace N2Engine::Audio
{
    namespace
    {
        /// Frames rendered per alcRenderSamplesSOFT call while pacing (bounds the scratch buffer)
        constexpr std::uint32_t StreamChunkFrames = 1024;

        std::uint32_t StreamCapacityFrames(const LoopbackFormat &format)
        {
            return format.sampleRate * AudioSystem::StreamBufferMilliseconds / 1000u;
        }
    }

    AudioSystem& AudioSystem::Instance()
    {
        // Never destroyed: AudioSources unregister in their destructors, which can run during static
        // destruction (SceneManager's loaded scene). When AudioSystem was first used after SceneManager
        // was created (headless, where Init skips audio), a function-local static died first.
        static AudioSystem *instance = new AudioSystem();
        return *instance;
    }

    bool AudioSystem::Initialize(AudioOutput output)
    {
        if (_initialized)
        {
            if (output != _output)
            {
                Logger::Warn("AudioSystem is already initialized with another output; call Shutdown first");
                return false;
            }
            return true;
        }

        _output = output;
        const bool opened = output == AudioOutput::Loopback ? OpenLoopbackDevice() : OpenDevice();
        if (!opened || !FinishInitialize())
        {
            ResetLoopbackState();
            _output = AudioOutput::Device;
            return false;
        }
        return true;
    }

    bool AudioSystem::OpenDevice()
    {
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
        return true;
    }

    bool AudioSystem::IsLoopbackSupported()
    {
        // A device-independent extension, so it can be asked without a device
        return alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback") == ALC_TRUE;
    }

    bool AudioSystem::OpenLoopbackDevice()
    {
        if (!IsLoopbackSupported())
        {
            Logger::Error("OpenAL has no ALC_SOFT_loopback extension; audio is disabled (nothing to stream)");
            return false;
        }

        // Extension entry points are loaded at run time (the import library needn't export them)
        const auto openLoopback = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
            alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
        const auto isFormatSupported = reinterpret_cast<LPALCISRENDERFORMATSUPPORTEDSOFT>(
            alcGetProcAddress(nullptr, "alcIsRenderFormatSupportedSOFT"));
        const auto renderSamples = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
            alcGetProcAddress(nullptr, "alcRenderSamplesSOFT"));
        if (!openLoopback || !isFormatSupported || !renderSamples)
        {
            Logger::Error("OpenAL reports ALC_SOFT_loopback but lacks its functions; audio is disabled");
            return false;
        }

        ALCdevice *device = openLoopback(nullptr);
        if (!device)
        {
            Logger::Error("Failed to open an OpenAL loopback device; audio is disabled");
            return false;
        }

        // Float keeps the mix's headroom and needs no dither; 16-bit is the fallback every build supports
        constexpr auto rate = static_cast<ALCsizei>(LoopbackSampleRate);
        static_assert(LoopbackChannels == 2, "the loopback device is opened as ALC_STEREO_SOFT");
        LoopbackFormat format{.sampleRate = LoopbackSampleRate, .channels = LoopbackChannels};
        ALCenum type = ALC_FLOAT_SOFT;
        if (isFormatSupported(device, rate, ALC_STEREO_SOFT, ALC_FLOAT_SOFT) == ALC_TRUE)
        {
            format.sampleFormat = SampleFormat::Float32;
        }
        else if (isFormatSupported(device, rate, ALC_STEREO_SOFT, ALC_SHORT_SOFT) == ALC_TRUE)
        {
            format.sampleFormat = SampleFormat::Int16;
            type = ALC_SHORT_SOFT;
        }
        else
        {
            Logger::Error("The OpenAL loopback device supports neither float32 nor int16 stereo at 48 kHz; audio is disabled");
            alcCloseDevice(device);
            return false;
        }

        const ALCint attributes[] = {
            ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
            ALC_FORMAT_TYPE_SOFT, type,
            ALC_FREQUENCY, rate,
            0,
        };
        _context = alcCreateContext(device, attributes);
        if (!_context)
        {
            Logger::Error("Failed to create an OpenAL context on the loopback device; audio is disabled");
            alcCloseDevice(device);
            return false;
        }

        _device = device;
        _renderSamples = renderSamples;
        _loopbackFormat = format;
        _streamAccumulator = 0.0;
        _streamRing.assign(static_cast<std::size_t>(StreamCapacityFrames(format)) * format.BytesPerFrame(), 0);
        _streamStart = 0;
        _streamSize = 0;
        _streamDroppedFrames = 0;
        Logger::Info(std::format("OpenAL loopback device: {} Hz, {} channels, {}", format.sampleRate, format.channels,
                                 ToString(format.sampleFormat)));
        return true;
    }

    bool AudioSystem::FinishInitialize()
    {
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
        ResetLoopbackState();
        _output = AudioOutput::Device;
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

    void AudioSystem::ResetLoopbackState()
    {
        _renderSamples = nullptr;
        _loopbackFormat = {};
        _streamAccumulator = 0.0;
        _streamRing.clear();
        _streamRing.shrink_to_fit();
        _streamStart = 0;
        _streamSize = 0;
        _streamDroppedFrames = 0;
        _streamScratch.clear();
        _streamScratch.shrink_to_fit();
    }

    bool AudioSystem::RenderSamples(void *buffer, std::uint32_t frameCount)
    {
        if (!IsLoopback() || !_renderSamples || !buffer)
        {
            return false;
        }

        // ALCsizei is an int: render a larger request in pieces
        constexpr auto maxPerCall = static_cast<std::uint32_t>(std::numeric_limits<ALCsizei>::max() / 16);
        auto *out = static_cast<std::uint8_t *>(buffer);
        const std::size_t frameBytes = _loopbackFormat.BytesPerFrame();
        while (frameCount > 0)
        {
            const std::uint32_t frames = std::min(frameCount, maxPerCall);
            _renderSamples(_device, out, static_cast<ALCsizei>(frames));
            out += static_cast<std::size_t>(frames) * frameBytes;
            frameCount -= frames;
        }
        return true;
    }

    void AudioSystem::AdvanceStream(double unscaledSeconds)
    {
        // !(x > 0) also rejects NaN
        if (!IsLoopback() || !(unscaledSeconds > 0.0))
        {
            return;
        }

        _streamAccumulator += std::min(unscaledSeconds, MaxStreamAdvanceSeconds) *
                              static_cast<double>(_loopbackFormat.sampleRate);
        const double whole = std::floor(_streamAccumulator);
        _streamAccumulator -= whole;
        RenderIntoStream(static_cast<std::uint64_t>(whole));
    }

    void AudioSystem::RenderIntoStream(std::uint64_t frames)
    {
        const std::size_t frameBytes = _loopbackFormat.BytesPerFrame();
        _streamScratch.resize(static_cast<std::size_t>(StreamChunkFrames) * frameBytes);
        while (frames > 0)
        {
            // Every frame is rendered (that is what moves the clock), even those the buffer then discards
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, StreamChunkFrames));
            _renderSamples(_device, _streamScratch.data(), static_cast<ALCsizei>(chunk));
            PushStream(_streamScratch.data(), static_cast<std::size_t>(chunk) * frameBytes);
            frames -= chunk;
        }
    }

    void AudioSystem::PushStream(const std::uint8_t *data, std::size_t bytes)
    {
        const std::size_t capacity = _streamRing.size();
        const std::size_t frameBytes = _loopbackFormat.BytesPerFrame();
        if (capacity == 0 || bytes == 0)
        {
            return;
        }

        if (bytes >= capacity)
        {
            // Only the newest capacity bytes survive: everything held, and the head of data, is dropped
            _streamDroppedFrames += (_streamSize + (bytes - capacity)) / frameBytes;
            data += bytes - capacity;
            bytes = capacity;
            _streamStart = 0;
            _streamSize = 0;
        }
        else if (_streamSize + bytes > capacity)
        {
            // Discard the oldest frames to make room (capacity and bytes are whole frames)
            const std::size_t overflow = _streamSize + bytes - capacity;
            _streamStart = (_streamStart + overflow) % capacity;
            _streamSize -= overflow;
            _streamDroppedFrames += overflow / frameBytes;
        }

        const std::size_t writeAt = (_streamStart + _streamSize) % capacity;
        const std::size_t first = std::min(bytes, capacity - writeAt);
        std::memcpy(_streamRing.data() + writeAt, data, first);
        if (bytes > first)
        {
            std::memcpy(_streamRing.data(), data + first, bytes - first);
        }
        _streamSize += bytes;
    }

    StreamedAudio AudioSystem::TakeStreamedAudio()
    {
        StreamedAudio out;
        if (!IsLoopback())
        {
            return out;
        }

        const std::size_t capacity = _streamRing.size();
        out.samples.resize(_streamSize);
        if (_streamSize > 0)
        {
            const std::size_t first = std::min(_streamSize, capacity - _streamStart);
            std::memcpy(out.samples.data(), _streamRing.data() + _streamStart, first);
            if (_streamSize > first)
            {
                std::memcpy(out.samples.data() + first, _streamRing.data(), _streamSize - first);
            }
        }
        out.frameCount = static_cast<std::uint32_t>(_streamSize / _loopbackFormat.BytesPerFrame());
        out.droppedFrames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(_streamDroppedFrames, std::numeric_limits<std::uint32_t>::max()));

        _streamStart = 0;
        _streamSize = 0;
        _streamDroppedFrames = 0;
        return out;
    }

    void AudioSystem::ClearStream()
    {
        _streamStart = 0;
        _streamSize = 0;
        _streamDroppedFrames = 0;
    }

    std::uint32_t AudioSystem::GetStreamedFrameCount() const
    {
        if (!IsLoopback())
        {
            return 0;
        }
        return static_cast<std::uint32_t>(_streamSize / _loopbackFormat.BytesPerFrame());
    }

    std::uint32_t AudioSystem::GetStreamCapacityFrames() const
    {
        return IsLoopback() ? StreamCapacityFrames(_loopbackFormat) : 0u;
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