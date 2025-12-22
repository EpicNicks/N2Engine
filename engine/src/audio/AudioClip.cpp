#include "engine/audio/AudioClip.hpp"

#include "engine/Logger.hpp"

namespace N2Engine::Audio
{
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
        _sampleRate = data.sampleRate;
        _channels = data.channels;
        _duration = static_cast<float>(data.samples.size()) /
                   (data.sampleRate * data.channels * (data.bitsPerSample / 8));

        return true;
    }

    void AudioClip::Unload()
    {
        if (_buffer != 0)
        {
            alDeleteBuffers(1, &_buffer);
            _buffer = 0;
        }
        _duration = 0.0f;
        _sampleRate = 0;
        _channels = 0;
    }
}