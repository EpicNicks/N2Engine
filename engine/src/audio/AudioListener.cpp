#include "engine/audio/AudioListener.hpp"

#include <math/Vector3.hpp>

#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace N2Engine::Audio
{
    // Nothing of its own to save, but it must be recreated when a scene loads
    REGISTER_COMPONENT(AudioListener)

    AudioListener::AudioListener(GameObject& gameObject)
        : Component(gameObject)
    {
    }

    std::string AudioListener::GetTypeName() const
    {
        return NAMEOF(AudioListener);
    }

    void AudioListener::OnLateUpdate()
    {
        if (!_isActive)
        {
            return;
        }

        Positionable *transform = GetGameObject().GetPositionable();
        if (!transform)
        {
            return; // no transform to follow (it used to dereference null here)
        }
        auto pos = transform->GetGlobalTransform().GetPosition();
        auto forward = transform->GetForward();
        auto up = transform->GetUp();

        AudioSystem::Instance().SetListenerPosition(pos.x, pos.y, pos.z);
        AudioSystem::Instance().SetListenerOrientation(
            forward.x, forward.y, forward.z,
            up.x, up.y, up.z
        );
    }
}