#pragma once

#include <engine/Component.hpp>
#include <engine/common/ScriptUtils.hpp>

namespace N2Engine
{
    class GameObject;
}

class Spin : public N2Engine::Component
{
public:
    float degreesPerSecond = 1.0f;

public:
    Spin(N2Engine::GameObject &gameObject);
    void OnAttach() override;
    void OnUpdate() override;

    std::string GetTypeName() const override { return NAMEOF(Spin); }
};