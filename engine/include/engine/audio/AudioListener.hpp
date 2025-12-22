#pragma once

#include "engine/Component.hpp"

namespace N2Engine::Audio
{
    class AudioListener : public Component
    {
    public:
        explicit AudioListener(GameObject& gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void OnLateUpdate() override;

        static constexpr bool IsSingleton = true;
    };
}