#pragma once

#include <vector>
#include <AL/al.h>

#include "engine/base/Asset.hpp"

namespace N2Engine::Audio
{
    struct AudioData
    {
        std::vector<char> samples;
        int sampleRate = 0;
        int channels = 0;
        int bitsPerSample = 16;
    };

    class AudioClip : public Base::Asset
    {
    public:
        AudioClip() = default;
        ~AudioClip() override;

        // Create from audio data
        bool CreateFromData(const AudioData &data);

        // Unload the audio buffer
        void Unload();
        bool IsLoaded() const { return _buffer != 0; }

        // Getters
        ALuint GetBuffer() const { return _buffer; }
        float GetDuration() const { return _duration; }
        int GetSampleRate() const { return _sampleRate; }
        int GetChannels() const { return _channels; }

    private:
        ALuint _buffer = 0;
        float _duration = 0.0f;
        int _sampleRate = 0;
        int _channels = 0;
    };
}