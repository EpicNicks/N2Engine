#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"

#include <format>
#include <stdexcept>

namespace
{
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
            "SetClip", [](const AudioSourceRef &source, const std::string &path)
            {
                source.Pin()->SetClip(LoadClip(path, "AudioSource:SetClip"));
            },
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
            "PlayOneShot", [](const std::string &path, const sol::optional<float> volume,
                              const sol::optional<float> pitch, const sol::optional<std::string> &group)
            {
                Audio::PlaybackParams params;
                params.volume = volume.value_or(1.0f);
                params.pitch = pitch.value_or(1.0f);
                if (group)
                {
                    params.mixerGroup = *group;
                }
                return Audio::AudioSystem::Instance().PlayOneShot(LoadClip(path, "Audio.PlayOneShot"), params).id;
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
    }
}