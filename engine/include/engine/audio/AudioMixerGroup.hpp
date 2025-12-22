#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace N2Engine::Audio
{
    class AudioSource;

    struct AudioMixerGroupSettings
    {
        float volume = 1.0f;
        float maxSourceVolume = 1.0f;
        float pitch = 1.0f;
        bool muted = false;
        std::optional<std::uint32_t> maxConcurrent = std::nullopt;
    };

    struct AudioMixerGroup
    {
        std::string name;
        AudioMixerGroupSettings settings;

        std::uint32_t currentlyPlaying = 0;
        std::vector<AudioSource*> activeSources;
        static AudioMixerGroup FromSettings(const std::string &name, const AudioMixerGroupSettings &settings)
        {
            return AudioMixerGroup{
                .name = name,
                .settings = settings,
            };
        }
    };
}
