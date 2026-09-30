#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioLoaders.hpp"
#include "engine/io/Resources.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::Audio
{
    // Registered as the .wav/.ogg/.mp3/.flac loader by AudioClip.cpp. (It used to register itself from
    // an anonymous namespace here, but nothing referenced this file, so the static library linker
    // dropped it and audio files could never be loaded by extension.)
    std::shared_ptr<Base::Asset> LoadAudioClipFromFile(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), ::tolower);

        // Choose the appropriate loader
        std::unique_ptr<IAudioLoader> loader;

        if (ext == ".wav")
        {
            loader = std::make_unique<WAVLoader>();
        }
        else if (ext == ".ogg")
        {
            loader = std::make_unique<OGGLoader>();
        }
        else if (ext == ".mp3")
        {
            loader = std::make_unique<MP3Loader>();
        }
        else if (ext == ".flac")
        {
            loader = std::make_unique<FLACLoader>();
        }
        else
        {
            Logger::Error(std::format("Unsupported audio format: {}", ext));
            return nullptr;
        }

        // Load the raw audio data
        AudioData audioData = loader->Load(path);
        if (audioData.samples.empty())
        {
            return nullptr;
        }

        // Create AudioClip from the data
        auto clip = std::make_shared<AudioClip>();
        if (!clip->CreateFromData(audioData))
        {
            return nullptr;
        }

        return clip;
    }
}