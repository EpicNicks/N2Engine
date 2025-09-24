#pragma once

#include <engine/Component.hpp>

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
    void OnUpdate() override;
    void OnAttach() override;
};