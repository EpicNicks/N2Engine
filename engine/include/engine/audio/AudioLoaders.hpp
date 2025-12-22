#pragma once

#include <filesystem>
#include <memory>

#include "engine/audio/AudioClip.hpp"

namespace N2Engine::Audio
{
    // Base audio loader interface
    class IAudioLoader
    {
    public:
        virtual ~IAudioLoader() = default;
        virtual AudioData Load(const std::filesystem::path &path) = 0;
    };

    // WAV file loader (using dr_wav)
    class WAVLoader : public IAudioLoader
    {
    public:
        AudioData Load(const std::filesystem::path &path) override;
    };

    // OGG Vorbis file loader (using stb_vorbis)
    class OGGLoader : public IAudioLoader
    {
    public:
        AudioData Load(const std::filesystem::path &path) override;
    };

    // MP3 file loader (using dr_mp3)
    class MP3Loader : public IAudioLoader
    {
    public:
        AudioData Load(const std::filesystem::path &path) override;
    };

    // FLAC file loader (using dr_flac)
    class FLACLoader : public IAudioLoader
    {
    public:
        AudioData Load(const std::filesystem::path &path) override;
    };
}