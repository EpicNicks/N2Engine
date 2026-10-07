#include "engine/scripting/bindings/LuaBindings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "engine/GameObjectScene.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIText.hpp"

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // Components reach Lua only as ComponentRef<T> handles, never as raw pointers a script could keep
        struct ComponentAccess
        {
            std::function<sol::object(GameObject &, sol::this_state)> add;
            std::function<sol::object(const GameObject &, sol::this_state)> get;
            std::function<sol::object(Component &, lua_State *)> asLua; // null object if not this type
            std::function<std::optional<ComponentRefBase>(const sol::object &)> asRef; // nullopt if not this type
        };

        template <typename T>
        ComponentAccess MakeAccess()
        {
            return {
                [](GameObject &go, sol::this_state state)
                {
                    return sol::make_object(state, ComponentRef<T>(*go.AddComponent<T>()));
                },
                [](const GameObject &go, sol::this_state state) -> sol::object
                {
                    if (T *component = go.GetComponent<T>())
                    {
                        return sol::make_object(state, ComponentRef<T>(*component));
                    }
                    return sol::lua_nil;
                },
                [](Component &component, lua_State *state) -> sol::object
                {
                    if (auto *typed = dynamic_cast<T *>(&component))
                    {
                        return sol::make_object(state, ComponentRef<T>(*typed));
                    }
                    return sol::lua_nil;
                },
                [](const sol::object &object) -> std::optional<ComponentRefBase>
                {
                    if (object.is<ComponentRef<T>>())
                    {
                        return ComponentRefBase(object.as<ComponentRef<T>>());
                    }
                    return std::nullopt;
                },
            };
        }

        // Every type here needs a Lua usertype (see BindPhysics, BindAudio, BindComponents and BindUI)
        const std::map<std::string, ComponentAccess> &ComponentTable()
        {
            static const std::map<std::string, ComponentAccess> table{
                {"Rigidbody", MakeAccess<Physics::Rigidbody>()},
                {"BoxCollider", MakeAccess<Physics::BoxCollider>()},
                {"SphereCollider", MakeAccess<Physics::SphereCollider>()},
                {"CapsuleCollider", MakeAccess<Physics::CapsuleCollider>()},
                {"CubeRenderer", MakeAccess<Example::CubeRenderer>()},
                {"SphereRenderer", MakeAccess<Example::SphereRenderer>()},
                {"QuadRenderer", MakeAccess<Example::QuadRenderer>()},
                {"MeshRenderer", MakeAccess<Rendering::MeshRenderer>()},
                {"TextRenderer", MakeAccess<Rendering::TextRenderer>()},
                {"Light", MakeAccess<Rendering::Light>()},
                {"AudioSource", MakeAccess<Audio::AudioSource>()},
                {"AudioListener", MakeAccess<Audio::AudioListener>()},
                {"LuaComponent", MakeAccess<LuaComponent>()},
                {"RectTransform", MakeAccess<UI::RectTransform>()},
                {"Canvas", MakeAccess<UI::Canvas>()},
                {"Image", MakeAccess<UI::Image>()},
                {"UIText", MakeAccess<UI::UIText>()},
                {"Button", MakeAccess<UI::Button>()},
            };
            return table;
        }

        const ComponentAccess &FindAccess(const std::string &typeName)
        {
            const auto &table = ComponentTable();
            if (const auto it = table.find(typeName); it != table.end())
            {
                return it->second;
            }

            std::string known;
            for (const auto &name : table | std::views::keys)
            {
                known += known.empty() ? name : ", " + name;
            }
            // sol turns this into a Lua error at the call site
            throw std::runtime_error(std::format("Unknown component type '{}'. Known types: {}", typeName, known));
        }

        /// What scripts see as an asset's path: its res:// (or user://) path for a project asset, else `fallback`
        /// (the file it was loaded from, or "" for one made at runtime). A model's sub-asset is its model's path, '#'
        /// and its key: "res://models/robot.glb#mesh/Body".
        std::string AssetPathForLua(const Base::Asset &asset, const std::string &fallback)
        {
            if (asset.GetResourcePath().IsValid())
            {
                if (asset.IsSubResource())
                {
                    return asset.GetResourcePath().ToString() + "#" + asset.GetSubAssetKey();
                }
                return asset.GetResourcePath().ToString();
            }
            return fallback;
        }

        /**
         * A sub-asset reference, "res://models/robot.glb#mesh/Body": the model file before the '#' is loaded (or
         * reused) and its sub-asset with the key after it returned (nullptr if there is none, or it isn't a T).
         * nullopt for a reference with no '#' right after a ".glb" or ".gltf" (any case), which is an ordinary path
         * (a file or folder name may contain '#').
         */
        template <typename T>
        std::optional<std::shared_ptr<T>> LoadSubAssetReference(const std::string &reference)
        {
            std::size_t hash = std::string::npos;
            for (std::size_t at = reference.find('#'); at != std::string::npos; at = reference.find('#', at + 1))
            {
                std::string before = reference.substr(0, at);
                std::ranges::transform(before, before.begin(),
                                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (before.ends_with(".glb") || before.ends_with(".gltf"))
                {
                    hash = at;
                    break;
                }
            }
            if (hash == std::string::npos)
            {
                return std::nullopt;
            }
            const auto parent =
                IO::Resources::Instance().Load<Base::Asset>(std::filesystem::path(reference.substr(0, hash)));
            if (!parent)
            {
                return std::shared_ptr<T>{};
            }
            return std::dynamic_pointer_cast<T>(parent->FindSubAsset(std::string_view(reference).substr(hash + 1)));
        }

        /// "Directional", "Point" or "Spot"; anything else is an error naming `caller`
        Rendering::LightType ParseLightType(const std::string &name, const std::string_view caller)
        {
            if (name == "Directional") return Rendering::LightType::Directional;
            if (name == "Point") return Rendering::LightType::Point;
            if (name == "Spot") return Rendering::LightType::Spot;
            throw std::runtime_error(
                std::format("{}: unknown light type '{}'. Expected Directional, Point or Spot", caller, name));
        }

        std::string LightTypeName(const Rendering::LightType type)
        {
            switch (type)
            {
            case Rendering::LightType::Point: return "Point";
            case Rendering::LightType::Spot: return "Spot";
            case Rendering::LightType::Directional:
            default: return "Directional";
            }
        }

        /// A material slot's index from Lua (1-based) as a 0-based index; raises a Lua error below 1
        std::size_t MaterialSlot(const std::int64_t index, const std::string_view caller)
        {
            if (index < 1)
            {
                throw std::runtime_error(std::format("{}: material slots count from 1, got {}", caller, index));
            }
            return static_cast<std::size_t>(index - 1);
        }

        sol::optional<std::string> MaterialPathOrNil(const std::shared_ptr<Rendering::Material> &material)
        {
            if (!material)
            {
                return sol::nullopt;
            }
            return AssetPathForLua(*material, material->GetSourcePath());
        }

        /// The Lua methods every built-in shape has, on its handle type
        template <typename Ref>
        auto ShapeSetMaterial()
        {
            // A .mat file, e.g. "res://materials/crate.mat"; nil goes back to unlit in the shape's colour
            return [](const Ref &c, sol::optional<std::string> path)
            {
                const auto shape = c.Pin();
                if (!path)
                {
                    shape->SetMaterial(nullptr);
                    return;
                }
                shape->SetMaterial(LoadMaterialOrThrow(*path, "SetMaterial"));
            };
        }

        template <typename Ref>
        auto ShapeGetMaterial()
        {
            return [](const Ref &c) { return MaterialPathOrNil(c.Pin()->GetMaterial()); };
        }
    }

    std::shared_ptr<Rendering::Material> LoadMaterialOrThrow(const std::string &path, const std::string_view caller)
    {
        auto subAsset = LoadSubAssetReference<Rendering::Material>(path);
        auto material = subAsset ? std::move(*subAsset)
                                 : IO::Resources::Instance().Load<Rendering::Material>(std::filesystem::path(path));
        if (!material)
        {
            throw std::runtime_error(std::format("{}: can't load material '{}'", caller, path));
        }
        return material;
    }

    std::shared_ptr<Rendering::Mesh> LoadMeshOrThrow(const std::string &nameOrPath, const std::string_view caller)
    {
        if (const auto builtin = Rendering::Mesh::ParseBuiltinName(nameOrPath))
        {
            return Rendering::Mesh::GetBuiltin(*builtin);
        }
        auto subAsset = LoadSubAssetReference<Rendering::Mesh>(nameOrPath);
        auto mesh = subAsset ? std::move(*subAsset)
                             : IO::Resources::Instance().Load<Rendering::Mesh>(std::filesystem::path(nameOrPath));
        if (!mesh)
        {
            throw std::runtime_error(std::format(
                "{}: can't load mesh '{}' (the built-in meshes are \"Cube\", \"Sphere\" and \"Quad\")", caller,
                nameOrPath));
        }
        return mesh;
    }

    std::string MeshNameForLua(const Rendering::Mesh &mesh)
    {
        if (mesh.IsBuiltin())
        {
            return mesh.GetDebugName();
        }
        return AssetPathForLua(mesh, "");
    }

    // Text alignments travel as their scene-file names ("Left", "Middle", ...)
    Text::HorizontalAlign ParseHorizontalAlign(const std::string &name)
    {
        if (name == "Left") return Text::HorizontalAlign::Left;
        if (name == "Center") return Text::HorizontalAlign::Center;
        if (name == "Right") return Text::HorizontalAlign::Right;
        throw std::runtime_error(
            std::format("Unknown horizontal alignment '{}'. Expected Left, Center or Right", name));
    }

    Text::VerticalAlign ParseVerticalAlign(const std::string &name)
    {
        if (name == "Top") return Text::VerticalAlign::Top;
        if (name == "Middle") return Text::VerticalAlign::Middle;
        if (name == "Bottom") return Text::VerticalAlign::Bottom;
        if (name == "Baseline") return Text::VerticalAlign::Baseline;
        throw std::runtime_error(
            std::format("Unknown vertical alignment '{}'. Expected Top, Middle, Bottom or Baseline", name));
    }

    std::string AlignName(const Text::HorizontalAlign align)
    {
        switch (align)
        {
        case Text::HorizontalAlign::Center: return "Center";
        case Text::HorizontalAlign::Right: return "Right";
        case Text::HorizontalAlign::Left:
        default: return "Left";
        }
    }

    std::string AlignName(const Text::VerticalAlign align)
    {
        switch (align)
        {
        case Text::VerticalAlign::Middle: return "Middle";
        case Text::VerticalAlign::Bottom: return "Bottom";
        case Text::VerticalAlign::Baseline: return "Baseline";
        case Text::VerticalAlign::Top:
        default: return "Top";
        }
    }

    std::shared_ptr<Text::Font> LoadFontOrThrow(const std::string &path, const std::string_view caller)
    {
        auto font = IO::Resources::Instance().Load<Text::Font>(std::filesystem::path(path));
        if (!font || !font->IsLoaded())
        {
            throw std::runtime_error(std::format("{}: can't load font '{}'", caller, path));
        }
        return font;
    }

    std::shared_ptr<Rendering::Texture> LoadTextureOrThrow(const std::string &path, const std::string_view caller)
    {
        auto subAsset = LoadSubAssetReference<Rendering::Texture>(path);
        auto texture = subAsset ? std::move(*subAsset)
                                : IO::Resources::Instance().Load<Rendering::Texture>(std::filesystem::path(path));
        if (!texture || !texture->IsLoaded())
        {
            throw std::runtime_error(std::format("{}: can't load texture '{}'", caller, path));
        }
        return texture;
    }

    std::string TexturePathForLua(const Rendering::Texture &texture)
    {
        return AssetPathForLua(texture, texture.GetSourcePath());
    }

    sol::object AddComponentByName(GameObject &gameObject, const std::string &typeName, sol::this_state state)
    {
        return FindAccess(typeName).add(gameObject, state);
    }

    sol::object GetComponentByName(const GameObject &gameObject, const std::string &typeName, sol::this_state state)
    {
        return FindAccess(typeName).get(gameObject, state);
    }

    sol::object ComponentToLua(Component &component, lua_State *state)
    {
        for (const auto &access : ComponentTable() | std::views::values)
        {
            if (sol::object typed = access.asLua(component, state); typed.valid())
            {
                return typed;
            }
        }
        return sol::make_object(state, ComponentRef<Component>(component));
    }

    bool SameComponent(const sol::object &a, const sol::object &b)
    {
        const auto asRef = [](const sol::object &object) -> std::optional<ComponentRefBase>
        {
            for (const auto &access : ComponentTable() | std::views::values)
            {
                if (auto ref = access.asRef(object))
                {
                    return ref;
                }
            }
            if (object.is<ComponentRef<Component>>())
            {
                return ComponentRefBase(object.as<ComponentRef<Component>>());
            }
            return std::nullopt;
        };

        const auto refA = asRef(a);
        const auto refB = asRef(b);
        return refA && refB && refA->RefersTo(*refB);
    }

    std::vector<std::string> GetScriptableComponentNames()
    {
        std::vector<std::string> names;
        for (const auto &name : ComponentTable() | std::views::keys)
        {
            names.push_back(name);
        }
        return names;
    }

    void BindComponents(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // Returned by ComponentToLua for components with no Lua type of their own
        BindComponentType<Component>(lua, "Component");

        using CubeRendererRef = ComponentRef<Example::CubeRenderer>;
        BindComponentType<Example::CubeRenderer>(
            lua, "CubeRenderer",
            "SetColor", Forward<CubeRendererRef, &Example::CubeRenderer::SetColor>(),
            "GetColor", Forward<CubeRendererRef, &Example::CubeRenderer::GetColor>(),
            "SetSize", Forward<CubeRendererRef, &Example::CubeRenderer::SetSize>(),
            "GetSize", Forward<CubeRendererRef, &Example::CubeRenderer::GetSize>(),
            "SetMaterial", ShapeSetMaterial<CubeRendererRef>(),
            "GetMaterial", ShapeGetMaterial<CubeRendererRef>()
        );

        using SphereRendererRef = ComponentRef<Example::SphereRenderer>;
        BindComponentType<Example::SphereRenderer>(
            lua, "SphereRenderer",
            "SetColor", Forward<SphereRendererRef, &Example::SphereRenderer::SetColor>(),
            "GetColor", Forward<SphereRendererRef, &Example::SphereRenderer::GetColor>(),
            "SetRadius", Forward<SphereRendererRef, &Example::SphereRenderer::SetRadius>(),
            "GetRadius", Forward<SphereRendererRef, &Example::SphereRenderer::GetRadius>(),
            "SetSubdivision", Forward<SphereRendererRef, &Example::SphereRenderer::SetSubdivision>(),
            "SetMaterial", ShapeSetMaterial<SphereRendererRef>(),
            "GetMaterial", ShapeGetMaterial<SphereRendererRef>()
        );

        using QuadRendererRef = ComponentRef<Example::QuadRenderer>;
        BindComponentType<Example::QuadRenderer>(
            lua, "QuadRenderer",
            "SetColor", Forward<QuadRendererRef, &Example::QuadRenderer::SetColor>(),
            "GetColor", Forward<QuadRendererRef, &Example::QuadRenderer::GetColor>(),
            "SetSize", Forward<QuadRendererRef, &Example::QuadRenderer::SetSize>(),
            "GetSize", Forward<QuadRendererRef, &Example::QuadRenderer::GetSize>(),
            "SetMaterial", ShapeSetMaterial<QuadRendererRef>(),
            "GetMaterial", ShapeGetMaterial<QuadRendererRef>()
        );

        using MeshRendererRef = ComponentRef<Rendering::MeshRenderer>;
        BindComponentType<Rendering::MeshRenderer>(
            lua, "MeshRenderer",
            // A built-in mesh by name ("Cube", "Sphere", "Quad") or a mesh asset's path; nil clears it. A name or
            // path that doesn't load raises an error and keeps the current mesh.
            "SetMesh", [](const MeshRendererRef &c, sol::optional<std::string> nameOrPath)
            {
                const auto renderer = c.Pin();
                if (!nameOrPath)
                {
                    renderer->SetMesh(nullptr);
                    return;
                }
                renderer->SetMesh(LoadMeshOrThrow(*nameOrPath, "MeshRenderer:SetMesh"));
            },
            // The built-in mesh's name, or the mesh asset's path ("" for a mesh made at runtime); nil without a mesh
            "GetMesh", [](const MeshRendererRef &c) -> sol::optional<std::string>
            {
                const auto renderer = c.Pin();
                if (!renderer->GetMesh())
                {
                    return sol::nullopt;
                }
                return MeshNameForLua(*renderer->GetMesh());
            },
            "GetMaterialCount", [](const MeshRendererRef &c)
            {
                return static_cast<std::int64_t>(c.Pin()->GetMaterialCount());
            },
            // Slot i (1-based): a .mat file's path, or nil to empty the slot (drawn lit white)
            "SetMaterial", [](const MeshRendererRef &c, const std::int64_t index, sol::optional<std::string> path)
            {
                const std::size_t slot = MaterialSlot(index, "MeshRenderer:SetMaterial");
                const auto renderer = c.Pin();
                if (!path)
                {
                    renderer->SetMaterial(slot, nullptr);
                    return;
                }
                renderer->SetMaterial(slot, LoadMaterialOrThrow(*path, "MeshRenderer:SetMaterial"));
            },
            // Slot i's material path (1-based), or nil for an empty slot
            "GetMaterial", [](const MeshRendererRef &c, const std::int64_t index) -> sol::optional<std::string>
            {
                const std::size_t slot = MaterialSlot(index, "MeshRenderer:GetMaterial");
                return MaterialPathOrNil(c.Pin()->GetMaterial(slot));
            },
            // The mesh's bounds in its own space, or nil without a mesh
            "GetBounds", [](const MeshRendererRef &c) -> sol::optional<BoundingBox>
            {
                const auto renderer = c.Pin();
                if (const auto bounds = renderer->GetBounds())
                {
                    return *bounds;
                }
                return sol::nullopt;
            }
        );

        using TextRendererRef = ComponentRef<Rendering::TextRenderer>;
        BindComponentType<Rendering::TextRenderer>(
            lua, "TextRenderer",
            "SetText", Forward<TextRendererRef, &Rendering::TextRenderer::SetText>(),
            "GetText", Forward<TextRendererRef, &Rendering::TextRenderer::GetText>(),
            "SetColor", Forward<TextRendererRef, &Rendering::TextRenderer::SetColor>(),
            "GetColor", Forward<TextRendererRef, &Rendering::TextRenderer::GetColor>(),
            "SetFontSize", Forward<TextRendererRef, &Rendering::TextRenderer::SetFontSize>(),
            "GetFontSize", Forward<TextRendererRef, &Rendering::TextRenderer::GetFontSize>(),
            "SetMaxWidth", Forward<TextRendererRef, &Rendering::TextRenderer::SetMaxWidth>(),
            "GetMaxWidth", Forward<TextRendererRef, &Rendering::TextRenderer::GetMaxWidth>(),
            "SetLineSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::SetLineSpacing>(),
            "GetLineSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::GetLineSpacing>(),
            "SetLetterSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::SetLetterSpacing>(),
            "GetLetterSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::GetLetterSpacing>(),
            // Effects (Text::TextEffects), lengths in ems
            "SetOutline", [](const TextRendererRef &c, const float width, const Common::Color &color)
            {
                c.Pin()->SetOutline(width, color);
            },
            "GetOutline", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                const Text::TextEffects &effects = renderer->GetEffects();
                return std::make_tuple(effects.outlineWidth, Common::Color(effects.outlineColor));
            },
            "SetShadow", [](const TextRendererRef &c, const float offsetX, const float offsetY,
                            const Common::Color &color, const sol::optional<float> softness)
            {
                c.Pin()->SetShadow(Math::Vector2(offsetX, offsetY), color, softness.value_or(0.0f));
            },
            "GetShadow", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                const Text::TextEffects &effects = renderer->GetEffects();
                return std::make_tuple(effects.shadowOffset.x, effects.shadowOffset.y,
                                       Common::Color(effects.shadowColor), effects.shadowSoftness);
            },
            "SetSoftness", Forward<TextRendererRef, &Rendering::TextRenderer::SetSoftness>(),
            "GetSoftness", [](const TextRendererRef &c) { return c.Pin()->GetEffects().softness; },
            // Both names are checked before either is applied, so a bad one changes nothing
            "SetAlignment", [](const TextRendererRef &c, const std::string &horizontal, const std::string &vertical)
            {
                const Text::HorizontalAlign h = ParseHorizontalAlign(horizontal);
                const Text::VerticalAlign v = ParseVerticalAlign(vertical);
                const auto renderer = c.Pin();
                renderer->SetHorizontalAlign(h);
                renderer->SetVerticalAlign(v);
            },
            "GetAlignment", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                return std::make_tuple(AlignName(renderer->GetHorizontalAlign()),
                                       AlignName(renderer->GetVerticalAlign()));
            },
            // A font file, e.g. "res://fonts/Title.ttf"; nil goes back to the default font. A path that
            // doesn't load raises an error and keeps the current font.
            "SetFont", [](const TextRendererRef &c, sol::optional<std::string> path)
            {
                const auto renderer = c.Pin();
                if (!path)
                {
                    renderer->SetFont(nullptr);
                    return;
                }
                renderer->SetFont(LoadFontOrThrow(*path, "TextRenderer:SetFont"));
            },
            // The laid-out block in local units: minX, minY, maxX, maxY (all 0 for empty text)
            "GetBounds", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                const Text::Rect &bounds = renderer->GetLayout().bounds;
                return std::make_tuple(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);
            }
        );

        // The scene collects every active Light each frame; a scene with none is lit by a default directional light
        // (Scene::CollectLighting)
        using LightRef = ComponentRef<Rendering::Light>;
        BindComponentType<Rendering::Light>(
            lua, "Light",
            // "Directional", "Point" or "Spot"; an unknown name raises an error and changes nothing
            "SetType", [](const LightRef &c, const std::string &type)
            {
                const Rendering::LightType parsed = ParseLightType(type, "Light:SetType");
                c.Pin()->type = parsed;
            },
            "GetType", [](const LightRef &c) { return LightTypeName(c.Pin()->type); },
            // A directional light's direction of travel in world space (need not be normalized)
            "SetDirection", [](const LightRef &c, const Math::Vector3 &direction) { c.Pin()->direction = direction; },
            "GetDirection", [](const LightRef &c) { return c.Pin()->direction; },
            // The colour's r, g and b (alpha is ignored); GetColor returns alpha 1
            "SetColor", [](const LightRef &c, const Common::Color &color)
            {
                c.Pin()->color = Math::Vector3(color.r, color.g, color.b);
            },
            "GetColor", [](const LightRef &c)
            {
                const Math::Vector3 rgb = c.Pin()->color;
                return Common::Color(rgb.x, rgb.y, rgb.z, 1.0f);
            },
            "SetIntensity", [](const LightRef &c, const float intensity) { c.Pin()->intensity = intensity; },
            "GetIntensity", [](const LightRef &c) { return c.Pin()->intensity; },
            // Point and spot lights: how far the light reaches, in world units
            "SetRange", [](const LightRef &c, const float range) { c.Pin()->range = range; },
            "GetRange", [](const LightRef &c) { return c.Pin()->range; }
        );

        using LuaComponentRef = ComponentRef<LuaComponent>;
        BindComponentType<LuaComponent>(
            lua, "LuaComponent",
            // Path to a script under the project's assets, e.g. "res://scripts/CameraController.lua"
            "SetScript", [](const LuaComponentRef &c, const std::string &path)
            {
                c.Pin()->SetScript(IO::ResourcePath(path));
            },
            "GetScriptPath", [](const LuaComponentRef &c) { return c.Pin()->GetScriptPath().ToString(); },
            "HasMissingScript", Forward<LuaComponentRef, &LuaComponent::HasMissingScript>()
        );

        // ===== Model (global): a model file's GameObject hierarchy =====
        // Model.Instantiate(path [, parent]): the root object of a new instance. Under `parent` when given (at its
        // origin); otherwise in no scene yet, owned by the script until a scene or parent holds it (as
        // GameObject.Create's are). A model that doesn't load raises an error.
        const auto instantiate = [](const std::string &path, GameObjectRef *parent) -> GameObjectRef
        {
            const auto model = IO::Resources::Instance().Load<Rendering::Model>(std::filesystem::path(path));
            if (!model || !model->IsLoaded())
            {
                throw std::runtime_error(std::format("Model.Instantiate: can't load model '{}'", path));
            }
            GameObject::Ptr root = model->Instantiate();
            if (!root)
            {
                throw std::runtime_error(std::format("Model.Instantiate: can't instantiate '{}'", path));
            }
            if (parent)
            {
                const GameObject::Ptr parentObject = parent->Pin();
                parentObject->AddChild(root, false);
                if (root->GetParent() == parentObject)
                {
                    return GameObjectRef(root); // the parent owns it
                }
            }
            return GameObjectRef::Owning(std::move(root));
        };
        lua["Model"] = lua.create_table_with(
            "Instantiate", sol::overload(
                [instantiate](const std::string &path) { return instantiate(path, nullptr); },
                [instantiate](const std::string &path, GameObjectRef *parent) { return instantiate(path, parent); }
            )
        );
    }
}
