#include "engine/audio/AudioClip.hpp"

#include "engine/Logger.hpp"
#include "engine/audio/AudioLoaders.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/io/Resources.hpp"

namespace N2Engine::Audio
{
    namespace
    {
        // Lives here because every program that uses audio links this file (AudioClip's own code).
        // Resources passes the loader on to ResourceLoader, so res:// paths and the asset scan see audio files.
        struct AudioClipLoaderRegistrar
        {
            AudioClipLoaderRegistrar()
            {
                for (const char *extension : {".wav", ".ogg", ".mp3", ".flac"})
                {
                    IO::Resources::Instance().RegisterLoader(extension, LoadAudioClipFromFile);
                }
            }
        } g_audioClipLoaderRegistrar;
    }

    AudioClip::~AudioClip()
    {
        Unload();
    }

    bool AudioClip::CreateFromData(const AudioData &data)
    {
        if (data.samples.empty())
        {
            Logger::Error("Cannot create AudioClip from empty data");
            return false;
        }

        // Determine OpenAL format
        ALenum format;
        if (data.channels == 1)
        {
            format = (data.bitsPerSample == 8) ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
        }
        else if (data.channels == 2)
        {
            format = (data.bitsPerSample == 8) ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
        }
        else
        {
            Logger::Error(std::format("Unsupported channel count: {}", data.channels));
            return false;
        }

        // Buffers need a current OpenAL context (none headless, or without an audio device)
        auto &audio = AudioSystem::Instance();
        if (!audio.IsInitialized())
        {
            Logger::Warn("Cannot create AudioClip: the AudioSystem is not initialized (no OpenAL context)");
            return false;
        }

        // Release any previous buffer and clear stale errors so the check below only sees ours
        Unload();
        alGetError();

        // Create OpenAL buffer
        alGenBuffers(1, &_buffer);
        alBufferData(_buffer, format, data.samples.data(),
                     static_cast<ALsizei>(data.samples.size()), data.sampleRate);

        if (alGetError() != AL_NO_ERROR)
        {
            Logger::Error("Failed to create OpenAL buffer");
            alDeleteBuffers(1, &_buffer);
            _buffer = 0;
            return false;
        }

        // Store metadata
        _contextGeneration = audio.GetContextGeneration();
        _sampleRate = data.sampleRate;
        _channels = data.channels;
        _duration = static_cast<float>(data.samples.size()) /
                   (data.sampleRate * data.channels * (data.bitsPerSample / 8));

        return true;
    }

    bool AudioClip::IsLoaded() const
    {
        // The id outlives its context: after Shutdown (and a later Initialize) it names nothing, or
        // another clip's buffer
        const auto &audio = AudioSystem::Instance();
        return _buffer != 0 && audio.IsInitialized() && audio.GetContextGeneration() == _contextGeneration;
    }

    void AudioClip::Unload()
    {
        if (_buffer != 0)
        {
            // Only in the context it was made in: after Shutdown there's no context, and after a
            // re-Initialize the id may name another clip's buffer. Either way this one is already gone.
            const auto &audio = AudioSystem::Instance();
            if (audio.IsInitialized() && audio.GetContextGeneration() == _contextGeneration)
            {
                alDeleteBuffers(1, &_buffer);
            }
            _buffer = 0;
        }
        _duration = 0.0f;
        _sampleRate = 0;
        _channels = 0;
    }
}