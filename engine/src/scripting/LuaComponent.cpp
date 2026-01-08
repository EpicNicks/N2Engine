#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Scripting
{
    LuaComponent::LuaComponent(GameObject &gameObject)
        : SerializableComponent(gameObject) {}

    void LuaComponent::SetScript(const IO::ResourcePath &path)
    {
        _scriptPath = path;
        _script = nullptr;
        _scriptInstance = sol::nil;

        // Use ResourceLoader and gracefully handle missing files
        auto scriptAsset = IO::ResourceLoader::Instance().Load<LuaScript>(path);
        if (!scriptAsset)
        {
            Logger::Warn(std::format("Script file not found or failed to load: {}", path.ToString()));
            _hasMissingScript = true;
            return;
        }

        _script = scriptAsset.get();
        _hasMissingScript = false;

        // Rest of initialization...
        InitializeScriptInstance();
        ExtractSerializableFields();
        InjectFieldsIntoScript();
        CacheLifecycleMethods();

        // Register reload callback
        std::string moduleName = LuaRuntime::Instance().PathToModuleName(path);
        LuaRuntime::Instance().RegisterReloadCallback(moduleName, [this]()
        {
            ReloadScript();
        });
    }

    void LuaComponent::InitializeScriptInstance()
    {
        if (!_script)
            return;

        auto &lua = LuaRuntime::Instance().GetState();

        auto result = lua.safe_script(_script->GetSourceCode(), sol::script_pass_on_error);

        if (!result.valid())
        {
            sol::error err = result;
            Logger::Error(std::format("Failed to load script: {}", err.what()));
            _hasMissingScript = true;
            return;
        }

        sol::table scriptClass;

        if (result.return_count() > 0 && result[0].is<sol::table>())
        {
            scriptClass = result[0];
        }
        else
        {
            Logger::Error("Script must return a table");
            _hasMissingScript = true;
            return;
        }

        _scriptInstance = lua.create_table();
        _scriptInstance[sol::metatable_key] = scriptClass;

        _scriptInstance["component"] = this;
        _scriptInstance["gameObject"] = std::ref(_gameObject);

        _hasMissingScript = false;
    }

    void LuaComponent::ExtractSerializableFields()
    {
        if (!_scriptInstance.valid())
            return;

        sol::optional<sol::table> fieldsTable = _scriptInstance["SerializableFields"];
        if (!fieldsTable)
            return;

        for (const auto &[key, value] : *fieldsTable)
        {
            std::string fieldName = key.as<std::string>();

            if (!_scriptData.contains(fieldName))
            {
                sol::table fieldDef = value.as<sol::table>();
                sol::object defaultVal = fieldDef["default"];

                if (defaultVal.is<float>())
                    _scriptData[fieldName] = defaultVal.as<float>();
                else if (defaultVal.is<int>())
                    _scriptData[fieldName] = defaultVal.as<int>();
                else if (defaultVal.is<bool>())
                    _scriptData[fieldName] = defaultVal.as<bool>();
                else if (defaultVal.is<std::string>())
                    _scriptData[fieldName] = defaultVal.as<std::string>();
                else if (defaultVal.is<Math::Vector3>())
                {
                    auto vec = defaultVal.as<Math::Vector3>();
                    _scriptData[fieldName] = {{"x", vec.x}, {"y", vec.y}, {"z", vec.z}};
                }
            }
        }
    }

    void LuaComponent::InjectFieldsIntoScript()
    {
        if (!_scriptInstance.valid())
            return;

        for (auto &[key, value] : _scriptData.items())
        {
            if (value.is_number_float())
                _scriptInstance[key] = value.get<float>();
            else if (value.is_number_integer())
                _scriptInstance[key] = value.get<int>();
            else if (value.is_boolean())
                _scriptInstance[key] = value.get<bool>();
            else if (value.is_string())
                _scriptInstance[key] = value.get<std::string>();
            else if (value.is_object() && value.contains("x"))
            {
                Math::Vector3 vec{
                    value["x"].get<float>(),
                    value["y"].get<float>(),
                    value["z"].get<float>()
                };
                _scriptInstance[key] = vec;
            }
        }
    }

    void LuaComponent::CacheLifecycleMethods()
    {
        if (!_scriptInstance.valid())
        {
            // Reset all flags if script is invalid
            _hasOnUpdate = false;
            _hasOnFixedUpdate = false;
            _hasOnLateUpdate = false;
            _hasOnCollisionEnter = false;
            _hasOnCollisionStay = false;
            _hasOnCollisionExit = false;
            _hasOnTriggerEnter = false;
            _hasOnTriggerStay = false;
            _hasOnTriggerExit = false;
            return;
        }

        _hasOnUpdate = _scriptInstance["OnUpdate"].valid();
        _hasOnFixedUpdate = _scriptInstance["OnFixedUpdate"].valid();
        _hasOnLateUpdate = _scriptInstance["OnLateUpdate"].valid();
        _hasOnCollisionEnter = _scriptInstance["OnCollisionEnter"].valid();
        _hasOnCollisionStay = _scriptInstance["OnCollisionStay"].valid();
        _hasOnCollisionExit = _scriptInstance["OnCollisionExit"].valid();
        _hasOnTriggerEnter = _scriptInstance["OnTriggerEnter"].valid();
        _hasOnTriggerStay = _scriptInstance["OnTriggerStay"].valid();
        _hasOnTriggerExit = _scriptInstance["OnTriggerExit"].valid();
    }

    template <typename... Args>
    void LuaComponent::CallLuaMethod(const std::string &methodName, Args &&... args)
    {
        if (_hasMissingScript)
            return; // Silently skip if script is missing

        sol::protected_function func = _scriptInstance[methodName];
        auto result = func(_scriptInstance, std::forward<Args>(args)...);

        if (!result.valid())
        {
            sol::error err = result;
            Logger::Error(std::format("Lua {} error in {}: {}",
                                      methodName,
                                      _scriptPath.ToString(),
                                      err.what()));
        }
    }

    void LuaComponent::OnAttach()
    {
        if (_hasMissingScript)
        {
            Logger::Warn(std::format("LuaComponent on '{}' has missing script: {}",
                                     _gameObject.GetName(),
                                     _scriptPath.ToString()));
            return;
        }

        if (_scriptInstance.valid() && _scriptInstance["OnAttach"].valid())
        {
            CallLuaMethod("OnAttach");
        }
    }

    void LuaComponent::OnUpdate()
    {
        if (_hasOnUpdate && !_hasMissingScript)
        {
            CallLuaMethod("OnUpdate");
        }
    }

    void LuaComponent::OnFixedUpdate()
    {
        if (_hasOnFixedUpdate && !_hasMissingScript)
        {
            CallLuaMethod("OnFixedUpdate");
        }
    }

    void LuaComponent::OnLateUpdate()
    {
        if (_hasOnLateUpdate && !_hasMissingScript)
        {
            CallLuaMethod("OnLateUpdate");
        }
    }

    void LuaComponent::OnDestroy()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnDestroy"].valid() && !_hasMissingScript)
        {
            CallLuaMethod("OnDestroy");
        }
    }

    void LuaComponent::OnEnable()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnEnable"].valid() && !_hasMissingScript)
        {
            CallLuaMethod("OnEnable");
        }
    }

    void LuaComponent::OnDisable()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnDisable"].valid() && !_hasMissingScript)
        {
            CallLuaMethod("OnDisable");
        }
    }

    void LuaComponent::OnCollisionEnter(const Physics::Collision &collision)
    {
        if (_hasOnCollisionEnter && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionEnter", collision);
        }
    }

    void LuaComponent::OnCollisionStay(const Physics::Collision &collision)
    {
        if (_hasOnCollisionStay && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionStay", collision);
        }
    }

    void LuaComponent::OnCollisionExit(const Physics::Collision &collision)
    {
        if (_hasOnCollisionExit && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionExit", collision);
        }
    }

    void LuaComponent::OnTriggerEnter(Physics::Trigger trigger)
    {
        if (_hasOnTriggerEnter && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerEnter", trigger);
        }
    }

    void LuaComponent::OnTriggerStay(Physics::Trigger trigger)
    {
        if (_hasOnTriggerStay && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerStay", trigger);
        }
    }

    void LuaComponent::OnTriggerExit(Physics::Trigger trigger)
    {
        if (_hasOnTriggerExit && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerExit", trigger);
        }
    }

    nlohmann::json LuaComponent::Serialize() const
    {
        auto j = SerializableComponent::Serialize();

        // Store script as UUID (preferred) with path fallback
        auto &loader = IO::ResourceLoader::Instance();
        Math::UUID scriptUUID = loader.GetUUID(_scriptPath);

        if (scriptUUID != Math::UUID::ZERO)
        {
            j["scriptUUID"] = scriptUUID.ToString();
        }
        else
        {
            j["scriptPath"] = _scriptPath;
        }

        j["scriptData"] = _scriptData;
        return j;
    }

    void LuaComponent::Deserialize(const nlohmann::json &j, ReferenceResolver *resolver)
    {
        SerializableComponent::Deserialize(j, resolver);

        if (j.contains("scriptData"))
        {
            _scriptData = j["scriptData"];
        }

        // Try UUID first (preferred)
        if (j.contains("scriptUUID"))
        {
            auto uuid = Math::UUID::FromString(j["scriptUUID"].get<std::string>());
            if (uuid.has_value())
            {
                // Resolve UUID to path
                auto *meta = IO::ResourceLoader::Instance().GetMetadata(uuid.value());
                if (meta)
                {
                    SetScript(meta->resourcePath);
                }
                else
                {
                    Logger::Warn(std::format("Script UUID not found: {}", uuid.value().ToString()));
                    _hasMissingScript = true;
                }
            }
        }
        // Fallback to path (for backward compatibility)
        else if (j.contains("scriptPath"))
        {
            IO::ResourcePath path = j["scriptPath"].get<IO::ResourcePath>();
            SetScript(path);
        }

        // Resolve references if resolver provided
        if (resolver)
        {
            resolver->AddPendingReference([this, j, resolver]()
            {
                ResolveReferences(j, resolver);
            });
        }
    }

    void LuaComponent::ResolveReferences(const nlohmann::json &j, ReferenceResolver *resolver)
    {
        if (!_scriptInstance.valid())
            return;

        sol::optional<sol::table> fieldsTable = _scriptInstance["SerializableFields"];
        if (!fieldsTable)
            return;

        _hasUnresolvedReferences = false;

        for (auto &[fieldName, value] : _scriptData.items())
        {
            // Check if this is a reference field
            if (value.is_object() && value.contains("$ref"))
            {
                auto refValue = value["$ref"];
                if (refValue.is_null())
                {
                    _scriptInstance[fieldName] = sol::nil;
                    continue;
                }

                std::string uuidStr = refValue.get<std::string>();
                auto uuid = Math::UUID::FromString(uuidStr);

                if (!uuid.has_value())
                {
                    Logger::Error(std::format("Invalid UUID in reference field '{}': {}",
                                              fieldName, uuidStr));
                    continue;
                }

                // Get field type from Lua
                sol::table fieldDef = (*fieldsTable)[fieldName];
                std::string fieldType = fieldDef["type"].get_or<std::string>("");

                // Resolve based on type
                if (fieldType == "GameObject")
                {
                    GameObject *go = resolver->FindGameObject(uuid.value());
                    if (go)
                    {
                        _scriptInstance[fieldName] = go;
                    }
                    else
                    {
                        _scriptInstance[fieldName] = sol::nil;
                        _hasUnresolvedReferences = true;
                        Logger::Warn(std::format("Failed to resolve GameObject reference for field '{}'", fieldName));
                    }
                }
                else if (IsComponentType(fieldType))
                {
                    Component *comp = resolver->FindComponent(uuid.value());
                    if (comp)
                    {
                        _scriptInstance[fieldName] = comp;
                    }
                    else
                    {
                        _scriptInstance[fieldName] = sol::nil;
                        _hasUnresolvedReferences = true;
                        Logger::Warn(std::format("Failed to resolve Component reference for field '{}'", fieldName));
                    }
                }
            }
        }
    }

    bool LuaComponent::IsComponentType(const std::string &type)
    {
        return type.size() > 9 && type.substr(type.size() - 9) == "Component";
    }

    void LuaComponent::SetScriptData(const nlohmann::json &data)
    {
        _scriptData = data;
        InjectFieldsIntoScript();
    }

    template <typename T>
    T LuaComponent::GetField(const std::string &fieldName, T defaultValue) const
    {
        if (_scriptData.contains(fieldName))
        {
            return _scriptData[fieldName].get<T>();
        }
        return defaultValue;
    }

    template <typename T>
    void LuaComponent::SetField(const std::string &fieldName, const T &value)
    {
        _scriptData[fieldName] = value;

        if (_scriptInstance.valid())
        {
            _scriptInstance[fieldName] = value;
        }
    }

    void LuaComponent::ReloadScript()
    {
        if (!_script)
        {
            // Try to reload the script if it was missing before
            SetScript(_scriptPath);
            return;
        }

        auto savedData = _scriptData;

        InitializeScriptInstance();
        ExtractSerializableFields();

        _scriptData = savedData;
        InjectFieldsIntoScript();
        CacheLifecycleMethods();

        Logger::Info(std::format("Reloaded script: {}", _scriptPath.ToString()));
    }

    // Add getter for missing script status
    bool LuaComponent::HasMissingScript() const
    {
        return _hasMissingScript;
    }

    const IO::ResourcePath& LuaComponent::GetScriptPath() const
    {
        return _scriptPath;
    }

    // Register with ComponentRegistry
    namespace
    {
        struct LuaComponentRegistrar
        {
            LuaComponentRegistrar()
            {
                ComponentRegistry::Instance().Register(
                    "LuaComponent",
                    [](GameObject &go) -> std::unique_ptr<Component>
                    {
                        return std::make_unique<LuaComponent>(go);
                    });
            }
        } g_luaComponentRegistrar;

        struct LuaScriptLoaderRegistrar
        {
            LuaScriptLoaderRegistrar()
            {
                IO::ResourceLoader::Instance().RegisterSimpleLoader<LuaScript>(".lua");
            }
        } g_luaScriptLoader;
    }
}
