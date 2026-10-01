#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioListener.hpp"

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
            "SetRolloffFactor", Forward<AudioSourceRef, &Audio::AudioSource::SetRolloffFactor>()
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
            }
        );
    }
}