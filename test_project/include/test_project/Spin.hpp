#pragma once

#include <engine/serialization/ComponentSerializer.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/common/ScriptUtils.hpp>

namespace N2Engine
{
    class GameObject;
}

class Spin : public N2Engine::SerializableComponent
{
public:
    float degreesPerSecond = 1.0f;

public:
    explicit Spin(N2Engine::GameObject &gameObject);
    void OnAttach() override;
    void OnUpdate() override;

    std::string GetTypeName() const override { return NAMEOF(Spin); }
};

REGISTER_COMPONENT(Spin)