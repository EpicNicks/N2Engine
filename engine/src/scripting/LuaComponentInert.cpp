// The LuaComponent of a build without the scripting module (N2ENGINE_USE_SCRIPTING is OFF; docs/project-modules.html). A scene,
// prefab or editor edit that has LuaComponents still loads: each keeps its script reference and its script data, saves them
// again as they were read, and runs nothing. One warning says so, the first time one is read. Compiled only without the module;
// with it, LuaComponent.cpp is the component.
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/io/Resources.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/JsonLimits.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace N2Engine::Scripting
{
    namespace
    {
        void WarnOnce()
        {
            static std::atomic<bool> warned{false};
            if (warned.exchange(true))
            {
                return;
            }
            Logger::Warn("A scene has LuaComponents, but this build has no scripting module (N2ENGINE_USE_SCRIPTING is OFF): "
                         "their scripts do not run. Their script references and data are kept and saved again as they were.");
        }
    }

    LuaComponent::LuaComponent(GameObject &gameObject) : SerializableComponent(gameObject) {}

    void LuaComponent::SetScriptData(const nlohmann::json &data)
    {
        _scriptData = data.is_object() ? JsonLimits::IterativeCopy(data) : nlohmann::json::object();
    }

    nlohmann::json LuaComponent::Serialize() const
    {
        auto j = SerializableComponent::Serialize();
        if (!_scriptUuid.is_null())
        {
            j["scriptUUID"] = _scriptUuid;
        }
        else
        {
            j["scriptPath"] = _scriptPath;
        }
        j["scriptData"] = JsonLimits::IterativeCopy(_scriptData);
        return j;
    }

    void LuaComponent::Deserialize(const nlohmann::json &j, ReferenceResolver *resolver)
    {
        SerializableComponent::Deserialize(j, resolver);
        WarnOnce();
        if (j.contains("scriptData") && !j["scriptData"].is_null())
        {
            _scriptData = JsonLimits::IterativeCopy(j["scriptData"]);
        }
        if (const auto uuid = j.find("scriptUUID"); uuid != j.end() && uuid->is_string())
        {
            _scriptUuid = *uuid;
        }
        else if (const auto path = j.find("scriptPath"); path != j.end() && path->is_string())
        {
            _scriptUuid = nullptr;
            _scriptPath = path->get<IO::ResourcePath>();
        }
    }

    std::vector<FieldInfo> LuaComponent::DescribeFields() const
    {
        FieldInfo script;
        script.name = "scriptUUID";
        script.displayName = "Script";
        script.kind = FieldKind::AssetRef;
        script.typeName = std::string(LuaScript::ResourceTypeName);
        script.assetType = script.typeName;
        std::vector<FieldInfo> fields;
        fields.push_back(std::move(script));
        return fields;
    }

    void LuaComponent::SetEditorFields(const nlohmann::json &values, ReferenceResolver *)
    {
        if (!values.is_object())
        {
            return;
        }
        if (const auto uuid = values.find("scriptUUID"); uuid != values.end() && uuid->is_string())
        {
            // An empty string is no script, as for the real component
            _scriptUuid = uuid->get<std::string>().empty() ? nlohmann::json(nullptr) : *uuid;
            _scriptPath = IO::ResourcePath();
        }
        if (const auto data = values.find("scriptData"); data != values.end() && data->is_object())
        {
            for (const auto &[key, value] : data->items())
            {
                _scriptData[key] = JsonLimits::IterativeCopy(value);
            }
        }
    }

    namespace
    {
        struct LuaComponentRegistrar
        {
            LuaComponentRegistrar()
            {
                ComponentRegistry::Instance().Register(
                    "LuaComponent",
                    [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<LuaComponent>(go); });
            }
        } g_luaComponentRegistrar;

        // A .lua file is still an asset of type LuaScript (a project's scripts are listed, packed and kept); nothing runs it
        struct LuaScriptLoaderRegistrar
        {
            LuaScriptLoaderRegistrar()
            {
                IO::Resources::Instance().RegisterSimpleLoader<LuaScript>(".lua");
            }
        } g_luaScriptLoader;
    }
}
