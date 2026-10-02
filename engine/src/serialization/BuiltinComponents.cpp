#include <memory>
#include <string>

#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"

namespace N2Engine
{
    namespace
    {
        template <typename T>
        void Add(ComponentRegistry &registry, const std::string &typeName)
        {
            registry.Register(typeName, [](GameObject &gameObject) -> std::unique_ptr<Component>
            {
                return std::make_unique<T>(gameObject);
            });
        }
    }

    // Every engine component a scene can contain. The registry's constructor calls this, so anything that
    // creates components by name links this file, and every type here with it. (A REGISTER_COMPONENT
    // static in a .cpp nothing referenced was dropped from the static engine library by the linker, and
    // in a header it only registered where the header was included.)
    // The names are what each type's GetTypeName returns, which is what scenes save.
    void RegisterBuiltinComponents(ComponentRegistry &registry)
    {
        Add<Physics::Rigidbody>(registry, "Rigidbody");
        Add<Physics::BoxCollider>(registry, "BoxCollider");
        Add<Physics::SphereCollider>(registry, "SphereCollider");
        Add<Physics::CapsuleCollider>(registry, "CapsuleCollider");
        Add<Rendering::Light>(registry, "Light");
        Add<Audio::AudioSource>(registry, "AudioSource");
        Add<Audio::AudioListener>(registry, "AudioListener");
        Add<Example::CubeRenderer>(registry, "CubeRenderer");
        Add<Example::SphereRenderer>(registry, "SphereRenderer");
        Add<Example::QuadRenderer>(registry, "QuadRenderer");
        Add<Rendering::TextRenderer>(registry, "TextRenderer");
        Add<Scripting::LuaComponent>(registry, "LuaComponent");
        Add<UI::Canvas>(registry, "Canvas");
        Add<UI::RectTransform>(registry, "RectTransform");
        Add<UI::Image>(registry, "Image");
    }
}
