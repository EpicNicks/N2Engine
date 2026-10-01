#pragma once

#include <cstdint>
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

        /// Needs an initialized AudioSystem (an OpenAL context); fails with a warning without one
        bool CreateFromData(const AudioData &data);

        std::string GetResourceType() const override { return "AudioClip"; }

        // Unload the audio buffer
        void Unload();
        /// Has a buffer in the current OpenAL context (false once the AudioSystem it was made under shut down)
        bool IsLoaded() const;

        // Getters
        ALuint GetBuffer() const { return _buffer; }
        float GetDuration() const { return _duration; }
        int GetSampleRate() const { return _sampleRate; }
        int GetChannels() const { return _channels; }

    private:
        ALuint _buffer = 0;
        std::uint32_t _contextGeneration = 0; // AudioSystem's, when the buffer was made
        float _duration = 0.0f;
        int _sampleRate = 0;
        int _channels = 0;
    };
}