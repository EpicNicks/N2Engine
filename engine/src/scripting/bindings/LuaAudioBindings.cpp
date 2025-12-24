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
        lua.new_usertype<Audio::AudioSource>("AudioSource",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>(),
            
            "Play", &Audio::AudioSource::Play,
            "Pause", &Audio::AudioSource::Pause,
            "Stop", &Audio::AudioSource::Stop,
            "IsPlaying", &Audio::AudioSource::IsPlaying,
            "IsPaused", &Audio::AudioSource::IsPaused,
            
            "SetVolume", &Audio::AudioSource::SetVolume,
            "GetVolume", &Audio::AudioSource::GetVolume,
            
            "SetPitch", &Audio::AudioSource::SetPitch,
            "GetPitch", &Audio::AudioSource::GetPitch,
            
            "SetLoop", &Audio::AudioSource::SetLoop,
            "GetLoop", &Audio::AudioSource::GetLoop,
            
            "SetSpatial", &Audio::AudioSource::SetSpatial,
            "GetSpatial", &Audio::AudioSource::GetSpatial,
            
            "SetMixerGroup", &Audio::AudioSource::SetMixerGroup,
            "GetMixerGroup", &Audio::AudioSource::GetMixerGroup,
            
            "SetMinDistance", &Audio::AudioSource::SetMinDistance,
            "SetMaxDistance", &Audio::AudioSource::SetMaxDistance,
            "SetRolloffFactor", &Audio::AudioSource::SetRolloffFactor
        );
        
        // ===== AudioListener =====
        lua.new_usertype<Audio::AudioListener>("AudioListener",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>()
        );
        
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