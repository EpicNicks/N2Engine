#include "engine/audio/AudioListener.hpp"

#include <math/Vector3.hpp>

#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Audio
{
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