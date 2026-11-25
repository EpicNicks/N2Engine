#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"

#include "engine/audio/AudioSynth.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <vector>

namespace
{
    /// A clip made in Lua (Audio.CreateTone, Audio.CreateClip): the AudioClip userdata scripts hold
    struct LuaAudioClip
    {
        std::shared_ptr<N2Engine::Audio::AudioClip> clip;
    };

    /// A clip by path ("res://audio/shoot.ogg"); a Lua error names the path when it does not load
    std::shared_ptr<N2Engine::Audio::AudioClip> LoadClip(const std::string &path, const char *function)
    {
        auto clip = N2Engine::IO::ResourceLoader::Instance().Load<N2Engine::Audio::AudioClip>(
            N2Engine::IO::ResourcePath(path));
        if (!clip)
        {
            throw std::runtime_error(std::format("{}: could not load the audio clip '{}'", function, path));
        }
        return clip;
    }

    /// A clip argument: a path (loaded through the resource cache) or an AudioClip made in Lua
    std::shared_ptr<N2Engine::Audio::AudioClip> ResolveClip(const sol::object &clip, const char *function)
    {
        if (clip.is<std::string>())
        {
            return LoadClip(clip.as<std::string>(), function);
        }
        if (clip.is<LuaAudioClip>())
        {
            return clip.as<const LuaAudioClip &>().clip;
        }
        throw std::runtime_error(std::format("{}: the clip must be a path or an AudioClip", function));
    }

    /// table[key] as a number: the default when absent, a Lua error when it is not a number
    double NumberField(const sol::table &table, const char *key, const double fallback, const char *function)
    {
        const sol::object value = table[key];
        if (!value.valid() || value.get_type() == sol::type::lua_nil)
        {
            return fallback;
        }
        if (value.get_type() != sol::type::number)
        {
            throw std::runtime_error(std::format("{}: '{}' must be a number", function, key));
        }
        return value.as<double>();
    }

    sol::optional<LuaAudioClip> MakeClip(const N2Engine::Audio::AudioData &data)
    {
        // No output (headless, no sound card, an audio-less build): there is no OpenAL context to hold a buffer
        if (!N2Engine::Audio::AudioSystem::Instance().IsInitialized())
        {
            return sol::nullopt;
        }
        auto clip = std::make_shared<N2Engine::Audio::AudioClip>();
        if (!clip->CreateFromData(data))
        {
            return sol::nullopt;
        }
        return LuaAudioClip{std::move(clip)};
    }
}

namespace N2Engine::Scripting::Bindings
{
    void BindAudio(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== AudioSource =====
        using AudioSourceRef = ComponentRef<Audio::AudioSource>;
        BindComponentType<Audio::AudioSource>(lua, "AudioSource",
            "Play", Forward<AudioSourceRef, &Audio::AudioSource::Play>(),
            "Pause", Forward<AudioSourceRef, &Audio::AudioSource::Pause>(),
            "UnPause", Forward<AudioSourceRef, &Audio::AudioSource::UnPause>(),
            "Stop", Forward<AudioSourceRef, &Audio::AudioSource::Stop>(),
            "IsPlaying", Forward<AudioSourceRef, &Audio::AudioSource::IsPlaying>(),
            "IsPaused", Forward<AudioSourceRef, &Audio::AudioSource::IsPaused>(),

            "SetVolume", Forward<AudioSourceRef, &Audio::AudioSource::SetVolume>(),
            "GetVolume", Forward<AudioSourceRef, &Audio::AudioSource::GetVolume>(),

            "SetPitch", Forward<AudioSourceRef, &Audio::AudioSource::SetPitch>(),
            "GetPitch", Forward<AudioSourceRef, &Audio::AudioSource::GetPitch>(),

            "SetLoop", Forward<AudioSourceRef, &Audio::AudioSource::SetLoop>(),
            "GetLoop", Forward<AudioSourceRef, &Audio::AudioSource::GetLoop>(),

            "SetSpatial", Forward<AudioSourceRef, &Audio::AudioSource::SetSpatial>(),
            "GetSpatial", Forward<AudioSourceRef, &Audio::AudioSource::GetSpatial>(),

            "SetMixerGroup", Forward<AudioSourceRef, &Audio::AudioSource::SetMixerGroup>(),
            "GetMixerGroup", Forward<AudioSourceRef, &Audio::AudioSource::GetMixerGroup>(),

            "SetMinDistance", Forward<AudioSourceRef, &Audio::AudioSource::SetMinDistance>(),
            "SetMaxDistance", Forward<AudioSourceRef, &Audio::AudioSource::SetMaxDistance>(),
            "SetRolloffFactor", Forward<AudioSourceRef, &Audio::AudioSource::SetRolloffFactor>(),

            // The clip to play, by path ("res://audio/music.ogg"); loaded through the resource cache. A clip that does
            // not load is a Lua error and leaves the source's clip as it was.
            // Or an AudioClip made with Audio.CreateTone / Audio.CreateClip.
            "SetClip", [](const AudioSourceRef &source, const sol::object &clip)
            {
                source.Pin()->SetClip(ResolveClip(clip, "AudioSource:SetClip"));
            },
            // Starts on the exact output sample of the DSP time (Audio.GetDspTime); a past time plays at once
            "PlayScheduled", [](const AudioSourceRef &source, const double dspTime)
            {
                source.Pin()->PlayScheduled(dspTime);
            },
            "GetTime", Forward<AudioSourceRef, &Audio::AudioSource::GetTime>(),
            "SetTime", Forward<AudioSourceRef, &Audio::AudioSource::SetTime>(),
            "HasClip", [](const AudioSourceRef &source) { return source.Pin()->GetClip() != nullptr; },
            "SetPlayOnAwake", Forward<AudioSourceRef, &Audio::AudioSource::SetPlayOnAwake>(),
            "GetPlayOnAwake", Forward<AudioSourceRef, &Audio::AudioSource::GetPlayOnAwake>()
        );

        // ===== AudioListener =====
        BindComponentType<Audio::AudioListener>(lua, "AudioListener");
        
        // ===== AudioSystem (global) =====
        lua["Audio"] = lua.create_table_with(
            "SetMasterVolume", [](float volume) {
                Audio::AudioSystem::Instance().SetMasterVolume(volume);
            },
            "GetMasterVolume", []() {
                return Audio::AudioSystem::Instance().GetMasterVolume();
            },
            "SetListenerPosition", [](float x, float y, float z) {
                Audio::AudioSystem::Instance().SetListenerPosition(x, y, z);
            },
            "SetListenerOrientation", [](float fx, float fy, float fz, float ux, float uy, float uz) {
                Audio::AudioSystem::Instance().SetListenerOrientation(fx, fy, fz, ux, uy, uz);
            },

            // Fire-and-forget playback: Audio.PlayOneShot(path [, volume [, pitch [, group]]]) returns a handle id,
            // 0 when the sound was dropped (no device, the group's concurrency limit, a hidden browser tab). Group
            // defaults to "SFX". Overlapping shots each get their own source.
            // The clip may also be an AudioClip made with Audio.CreateTone / Audio.CreateClip.
            "PlayOneShot", [](const sol::object &clip, const sol::optional<float> volume,
                              const sol::optional<float> pitch, const sol::optional<std::string> &group)
            {
                Audio::PlaybackParams params;
                params.volume = volume.value_or(1.0f);
                params.pitch = pitch.value_or(1.0f);
                if (group)
                {
                    params.mixerGroup = *group;
                }
                return Audio::AudioSystem::Instance().PlayOneShot(ResolveClip(clip, "Audio.PlayOneShot"), params).id;
            },
            // Audio.PlayOneShotAt(clip, dspTime [, volume [, group [, pitch]]]): PlayOneShot, started on the exact
            // output sample of dspTime (Audio.GetDspTime's clock). A time already reached plays at once.
            "PlayOneShotAt", [](const sol::object &clip, const double dspTime, const sol::optional<float> volume,
                                const sol::optional<std::string> &group, const sol::optional<float> pitch)
            {
                Audio::PlaybackParams params;
                params.volume = volume.value_or(1.0f);
                params.pitch = pitch.value_or(1.0f);
                if (group)
                {
                    params.mixerGroup = *group;
                }
                return Audio::AudioSystem::Instance()
                    .PlayOneShotAt(ResolveClip(clip, "Audio.PlayOneShotAt"), dspTime, params)
                    .id;
            },
            "GetOneShotTime", [](const std::uint32_t id)
            {
                return Audio::AudioSystem::Instance().GetOneShotTime(Audio::AudioHandle{id});
            },
            "StopOneShot", [](const std::uint32_t id) { Audio::AudioSystem::Instance().Stop(Audio::AudioHandle{id}); },
            "IsOneShotPlaying", [](const std::uint32_t id)
            {
                return Audio::AudioSystem::Instance().IsPlaying(Audio::AudioHandle{id});
            },

            // Mixer groups (the defaults are Master, Music, SFX, UI, Voice and Ambient). Volume is clamped to 0..1;
            // an unknown group is ignored by the setters and reads as volume 1, unmuted.
            "SetGroupVolume", [](const std::string &group, const float volume)
            {
                Audio::AudioSystem::Instance().SetGroupVolume(group, volume);
            },
            "GetGroupVolume", [](const std::string &group)
            {
                const auto *mixerGroup = Audio::AudioSystem::Instance().GetMixerGroup(group);
                return mixerGroup ? mixerGroup->settings.volume : 1.0f;
            },
            "SetGroupMuted", [](const std::string &group, const bool muted)
            {
                Audio::AudioSystem::Instance().SetGroupMuted(group, muted);
            },
            "IsGroupMuted", [](const std::string &group)
            {
                const auto *mixerGroup = Audio::AudioSystem::Instance().GetMixerGroup(group);
                return mixerGroup != nullptr && mixerGroup->settings.muted;
            },
            "HasGroup", [](const std::string &group)
            {
                return Audio::AudioSystem::Instance().GetMixerGroup(group) != nullptr;
            }
        );

        sol::table audio = lua["Audio"];

        // ===== DSP clock (issue #445) =====
        // The mixer's sample clock in seconds: samples mixed / output rate. Monotonic; 0 without an audio output. The
        // clock PlayOneShotAt and AudioSource:PlayScheduled use.
        audio.set_function("GetDspTime", [] { return Audio::AudioSystem::Instance().GetDspTime(); });
        audio.set_function("GetSampleRate", [] { return Audio::AudioSystem::Instance().GetOutputSampleRate(); });
        audio.set_function("IsSampleAccurate", [] { return Audio::AudioSystem::Instance().IsSampleAccurateScheduling(); });

        // ===== Mixer groups from Lua (issue #445) =====
        // Audio.CreateGroup(name [, {volume, maxVolume, pitch, muted, maxConcurrent}]): true, or false when the group
        // exists already (change it with the setters). Groups are flat; volume and maxVolume are clamped to 0..1, pitch
        // to 1/8..8, and maxConcurrent (a whole number, 1 or more) caps the sounds playing in it at once.
        audio.set_function("CreateGroup", [](const std::string &name, const sol::optional<sol::table> &settings)
        {
            auto &system = Audio::AudioSystem::Instance();
            if (name.empty())
            {
                throw std::runtime_error("Audio.CreateGroup: the group needs a name");
            }
            if (system.GetMixerGroup(name) != nullptr)
            {
                return false;
            }
            // Every field is read (and a wrong type raised) before the group exists
            constexpr const char *function = "Audio.CreateGroup";
            double volume = 1.0, maxVolume = 1.0, pitch = 1.0, maxConcurrent = 0.0;
            bool muted = false;
            if (settings)
            {
                const sol::table &table = *settings;
                volume = NumberField(table, "volume", volume, function);
                maxVolume = NumberField(table, "maxVolume", maxVolume, function);
                pitch = NumberField(table, "pitch", pitch, function);
                maxConcurrent = NumberField(table, "maxConcurrent", maxConcurrent, function);
                muted = table.get_or("muted", false);
            }
            system.CreateMixerGroup(name);
            system.SetGroupVolume(name, static_cast<float>(volume));
            system.SetGroupMaxVolume(name, static_cast<float>(maxVolume));
            system.SetGroupPitch(name, static_cast<float>(pitch));
            system.SetGroupMuted(name, muted);
            if (maxConcurrent >= 1.0)
            {
                system.SetGroupMaxConcurrent(name, static_cast<std::uint32_t>(std::min(maxConcurrent, 4096.0)));
            }
            return true;
        });
        // Audio.SetGroupMaxConcurrent(group, n): at most n sounds of the group play at once; nil (or 0) for no limit
        audio.set_function("SetGroupMaxConcurrent", [](const std::string &group, const sol::optional<double> max)
        {
            auto &system = Audio::AudioSystem::Instance();
            auto *mixerGroup = system.GetMixerGroup(group);
            if (mixerGroup == nullptr)
            {
                return;
            }
            if (!max || !(*max >= 1.0))
            {
                mixerGroup->settings.maxConcurrent.reset();
                return;
            }
            system.SetGroupMaxConcurrent(group, static_cast<std::uint32_t>(std::min(*max, 4096.0)));
        });
        audio.set_function("GetGroupMaxConcurrent", [](const std::string &group) -> sol::optional<std::uint32_t>
        {
            const auto *mixerGroup = Audio::AudioSystem::Instance().GetMixerGroup(group);
            if (mixerGroup == nullptr || !mixerGroup->settings.maxConcurrent)
            {
                return sol::nullopt;
            }
            return *mixerGroup->settings.maxConcurrent;
        });
        audio.set_function("SetGroupPitch", [](const std::string &group, const float pitch)
        {
            Audio::AudioSystem::Instance().SetGroupPitch(group, pitch);
        });
        audio.set_function("GetGroupPitch", [](const std::string &group)
        {
            const auto *mixerGroup = Audio::AudioSystem::Instance().GetMixerGroup(group);
            return mixerGroup ? mixerGroup->settings.pitch : 1.0f;
        });
        audio.set_function("SetGroupMaxVolume", [](const std::string &group, const float maxVolume)
        {
            Audio::AudioSystem::Instance().SetGroupMaxVolume(group, maxVolume);
        });

        // ===== Synthesis (issue #445) =====
        lua.new_usertype<LuaAudioClip>("AudioClip", sol::no_constructor,
            "GetDuration", [](const LuaAudioClip &c) { return c.clip ? c.clip->GetDuration() : 0.0f; },
            "GetFrameCount", [](const LuaAudioClip &c) { return c.clip ? c.clip->GetFrameCount() : std::uint64_t{0}; },
            "GetSampleRate", [](const LuaAudioClip &c) { return c.clip ? c.clip->GetSampleRate() : 0; },
            "GetChannels", [](const LuaAudioClip &c) { return c.clip ? c.clip->GetChannels() : 0; },
            "IsLoaded", [](const LuaAudioClip &c) { return c.clip && c.clip->IsLoaded(); });

        // Audio.CreateTone{waveform, frequency, duration, volume, attack, decay, sustain, release, sampleRate, seed}:
        // an AudioClip of one note (AudioSynth.hpp: GenerateTone). Invalid parameters are a Lua error; nil when there
        // is no audio output to hold the clip.
        audio.set_function("CreateTone", [](const sol::optional<sol::table> &options)
        {
            constexpr const char *function = "Audio.CreateTone";
            Audio::ToneParams params;
            if (options)
            {
                const sol::table &table = *options;
                const sol::object waveform = table["waveform"];
                if (waveform.valid() && waveform.get_type() != sol::type::lua_nil)
                {
                    const auto parsed = waveform.is<std::string>()
                                            ? Audio::WaveformFromString(waveform.as<std::string>())
                                            : std::nullopt;
                    if (!parsed)
                    {
                        throw std::runtime_error(std::format(
                            "{}: waveform must be \"sine\", \"square\", \"saw\", \"triangle\" or \"noise\"", function));
                    }
                    params.waveform = *parsed;
                }
                params.frequency = NumberField(table, "frequency", params.frequency, function);
                params.duration = NumberField(table, "duration", params.duration, function);
                params.volume = NumberField(table, "volume", params.volume, function);
                params.attack = NumberField(table, "attack", params.attack, function);
                params.decay = NumberField(table, "decay", params.decay, function);
                params.sustain = NumberField(table, "sustain", params.sustain, function);
                params.release = NumberField(table, "release", params.release, function);
                const double rate = NumberField(table, "sampleRate", params.sampleRate, function);
                params.sampleRate = rate >= 0.0 && rate <= 1.0e6 ? static_cast<int>(rate) : -1;
                const double seed = NumberField(table, "seed", params.seed, function);
                params.seed = seed >= 0.0 && seed <= 4294967295.0 ? static_cast<std::uint32_t>(seed) : 1u;
            }
            Audio::AudioData data;
            if (const auto error = Audio::GenerateTone(params, data))
            {
                throw std::runtime_error(std::format("{}: {}", function, *error));
            }
            return MakeClip(data);
        });

        // Audio.CreateClip(samples [, sampleRate [, channels]]): an AudioClip from a list of numbers (-1..1, clamped;
        // interleaved left, right for 2 channels). At most AudioSynth's MaxSynthSeconds long; invalid input is a Lua
        // error; nil when there is no audio output to hold the clip.
        audio.set_function("CreateClip", [](const sol::table &samples, const sol::optional<int> sampleRate,
                                            const sol::optional<int> channels)
        {
            constexpr const char *function = "Audio.CreateClip";
            const int rate = sampleRate.value_or(48000);
            const int channelCount = channels.value_or(1);
            const std::size_t count = samples.size();
            const auto limit = static_cast<std::size_t>(Audio::MaxSynthSeconds * Audio::MaxSynthSampleRate * 2);
            if (count > limit)
            {
                throw std::runtime_error(std::format("{}: too many samples ({})", function, count));
            }
            std::vector<float> values;
            values.reserve(count);
            for (std::size_t i = 1; i <= count; ++i)
            {
                const sol::object value = samples[i];
                if (value.get_type() != sol::type::number)
                {
                    throw std::runtime_error(std::format("{}: sample {} is not a number", function, i));
                }
                values.push_back(static_cast<float>(value.as<double>()));
            }
            Audio::AudioData data;
            if (const auto error = Audio::PcmFromFloats(values, rate, channelCount, data))
            {
                throw std::runtime_error(std::format("{}: {}", function, *error));
            }
            return MakeClip(data);
        });
    }
}