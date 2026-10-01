#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

#include <algorithm>
#include <cstdint>

namespace N2Engine::Scripting
{
    LuaComponent::LuaComponent(GameObject &gameObject)
        : SerializableComponent(gameObject) {}

    LuaComponent::~LuaComponent()
    {
        // Normally OnDestroy already did this; this covers components freed without it
        ReleaseScript();
    }

    void LuaComponent::ReleaseScript()
    {
        *_alive = false;
        LuaRuntime::Instance().UnregisterReloadCallbacks(this);

        // Closures the script registered can outlive this component (they keep `self` alive);
        // make those fields nil so a stray call errors in Lua instead of touching freed memory
        if (_scriptInstance.valid())
        {
            _scriptInstance["component"] = sol::lua_nil;
            _scriptInstance["gameObject"] = sol::lua_nil;
        }
    }

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

        // Register reload callback, replacing any from a previous SetScript; removed on destroy
        std::string moduleName = LuaRuntime::Instance().PathToModuleName(path);
        LuaRuntime::Instance().UnregisterReloadCallbacks(this);
        LuaRuntime::Instance().RegisterReloadCallback(moduleName, [this]()
        {
            ReloadScript();
        }, this);
    }

    void LuaComponent::InitializeScriptInstance()
    {
        if (!_script)
            return;

        auto &lua = LuaRuntime::Instance().GetState();

        auto result = LuaRuntime::Instance().RunSource(_script->GetSourceCode(), _scriptPath.ToString());

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

        // Handles, so anything that copies them out of self can't reach freed objects later
        _scriptInstance["component"] = ComponentRef<LuaComponent>(*this);
        _scriptInstance["gameObject"] = GameObjectRef(_gameObject);

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
                // Either { default = 5, ... } or the shorthand `speed = 5`. Casting the shorthand to a
                // table used to abort the process (sol has no safety checks enabled here).
                const sol::object defaultVal = value.is<sol::table>()
                                                   ? value.as<sol::table>().get<sol::object>("default")
                                                   : value;

                if (defaultVal.get_type() == sol::type::number)
                {
                    // Keep integers integral (they used to be stored as floats: 3 became 3.0)
                    lua_State *L = defaultVal.lua_state();
                    defaultVal.push(L);
                    const bool isInteger = lua_isinteger(L, -1);
                    lua_pop(L, 1);
                    if (isInteger)
                        _scriptData[fieldName] = defaultVal.as<std::int64_t>();
                    else
                        _scriptData[fieldName] = defaultVal.as<double>();
                }
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
                // Missing or non-numeric components default to 0 (a partial vector used to throw
                // out of scene loading)
                const auto component = [&value](const char *axis)
                {
                    return value.contains(axis) && value[axis].is_number() ? value[axis].get<float>() : 0.0f;
                };
                _scriptInstance[key] = Math::Vector3{component("x"), component("y"), component("z")};
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

        // Callbacks the script registers during this call (e.g. Subscribe in OnAttach) are tied to this component
        ScriptLifetimeScope scope(_alive);

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
        ReleaseScript();
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

    // Scripts get the collision with handles in place of its raw pointers, so keeping it is safe
    void LuaComponent::OnCollisionEnter(const Physics::Collision &collision)
    {
        if (_hasOnCollisionEnter && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionEnter", Bindings::CollisionToLua(collision, _scriptInstance.lua_state()));
        }
    }

    void LuaComponent::OnCollisionStay(const Physics::Collision &collision)
    {
        if (_hasOnCollisionStay && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionStay", Bindings::CollisionToLua(collision, _scriptInstance.lua_state()));
        }
    }

    void LuaComponent::OnCollisionExit(const Physics::Collision &collision)
    {
        if (_hasOnCollisionExit && !_hasMissingScript)
        {
            CallLuaMethod("OnCollisionExit", Bindings::CollisionToLua(collision, _scriptInstance.lua_state()));
        }
    }

    void LuaComponent::OnTriggerEnter(Physics::Trigger trigger)
    {
        if (_hasOnTriggerEnter && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerEnter", Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()));
        }
    }

    void LuaComponent::OnTriggerStay(Physics::Trigger trigger)
    {
        if (_hasOnTriggerStay && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerStay", Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()));
        }
    }

    void LuaComponent::OnTriggerExit(Physics::Trigger trigger)
    {
        if (_hasOnTriggerExit && !_hasMissingScript)
        {
            CallLuaMethod("OnTriggerExit", Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()));
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

                // Get field type from Lua (a shorthand field has no table, so no type)
                const sol::object fieldDefObject = (*fieldsTable)[fieldName];
                const std::string fieldType = fieldDefObject.is<sol::table>()
                                                  ? fieldDefObject.as<sol::table>().get_or<std::string>("type", "")
                                                  : std::string{};

                // Resolve based on type
                if (fieldType == "GameObject")
                {
                    GameObject *go = resolver->FindGameObject(uuid.value());
                    if (go)
                    {
                        _scriptInstance[fieldName] = GameObjectRef(*go);
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
                        // As its concrete Lua type (Rigidbody, BoxCollider, ...), not the Component base
                        _scriptInstance[fieldName] = Bindings::ComponentToLua(*comp, _scriptInstance.lua_state());
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
        // Any type scripts can add by name (Rigidbody, BoxCollider, ...), or a name ending in "Component".
        // Only the latter used to count, so `type = "Rigidbody"` reference fields were silently skipped.
        const auto names = Bindings::GetScriptableComponentNames();
        return std::ranges::find(names, type) != names.end() ||
               (type.size() > 9 && type.ends_with("Component"));
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

    // Defined here rather than in the header, so instantiate the field types scripts support
    template int LuaComponent::GetField<int>(const std::string &, int) const;
    template float LuaComponent::GetField<float>(const std::string &, float) const;
    template bool LuaComponent::GetField<bool>(const std::string &, bool) const;
    template std::string LuaComponent::GetField<std::string>(const std::string &, std::string) const;
    template void LuaComponent::SetField<int>(const std::string &, const int &);
    template void LuaComponent::SetField<float>(const std::string &, const float &);
    template void LuaComponent::SetField<bool>(const std::string &, const bool &);
    template void LuaComponent::SetField<std::string>(const std::string &, const std::string &);

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
                IO::Resources::Instance().RegisterSimpleLoader<LuaScript>(".lua");
            }
        } g_luaScriptLoader;
    }
}
