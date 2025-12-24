#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Scripting
{
    LuaComponent::LuaComponent(GameObject& gameObject)
        : SerializableComponent(gameObject)
    {
        // No need to register members since we handle serialization manually
    }
    
    void LuaComponent::SetScript(const IO::ResourcePath& path)
    {
        _scriptPath = path;
        
        auto scriptAsset = IO::ResourceLoader::Instance().Load<LuaScript>(path);
        if (!scriptAsset)
        {
            Logger::Error(std::format("Failed to load script: {}", path.ToString()));
            return;
        }
        
        _script = scriptAsset.get();
        
        InitializeScriptInstance();
        ExtractSerializableFields();
        InjectFieldsIntoScript();
        CacheLifecycleMethods();
        
        // Register reload callback
        std::string moduleName = LuaRuntime::Instance().PathToModuleName(path);
        LuaRuntime::Instance().RegisterReloadCallback(moduleName, [this]() {
            ReloadScript();
        });
    }
    
    void LuaComponent::InitializeScriptInstance()
    {
        if (!_script)
            return;
        
        auto& lua = LuaRuntime::Instance().GetState();
        
        auto result = lua.safe_script(_script->GetSourceCode(), sol::script_pass_on_error);
        
        if (!result.valid())
        {
            sol::error err = result;
            Logger::Error(std::format("Failed to load script: {}", err.what()));
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
            return;
        }
        
        _scriptInstance = lua.create_table();
        _scriptInstance[sol::metatable_key] = scriptClass;
        
        _scriptInstance["component"] = this;
        _scriptInstance["gameObject"] = std::ref(_gameObject);
    }
    
    void LuaComponent::ExtractSerializableFields()
    {
        if (!_scriptInstance.valid())
            return;
        
        sol::optional<sol::table> fieldsTable = _scriptInstance["SerializableFields"];
        if (!fieldsTable)
            return;
        
        for (const auto& [key, value] : *fieldsTable)
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
        
        for (auto& [key, value] : _scriptData.items())
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
            return;
        
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
    
    template<typename... Args>
    void LuaComponent::CallLuaMethod(const std::string& methodName, Args&&... args)
    {
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
        if (_scriptInstance.valid() && _scriptInstance["OnAttach"].valid())
        {
            CallLuaMethod("OnAttach");
        }
    }
    
    void LuaComponent::OnUpdate()
    {
        if (_hasOnUpdate)
        {
            CallLuaMethod("OnUpdate");
        }
    }
    
    void LuaComponent::OnFixedUpdate()
    {
        if (_hasOnFixedUpdate)
        {
            CallLuaMethod("OnFixedUpdate");
        }
    }
    
    void LuaComponent::OnLateUpdate()
    {
        if (_hasOnLateUpdate)
        {
            CallLuaMethod("OnLateUpdate");
        }
    }
    
    void LuaComponent::OnDestroy()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnDestroy"].valid())
        {
            CallLuaMethod("OnDestroy");
        }
    }
    
    void LuaComponent::OnEnable()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnEnable"].valid())
        {
            CallLuaMethod("OnEnable");
        }
    }
    
    void LuaComponent::OnDisable()
    {
        if (_scriptInstance.valid() && _scriptInstance["OnDisable"].valid())
        {
            CallLuaMethod("OnDisable");
        }
    }
    
    void LuaComponent::OnCollisionEnter(const Physics::Collision& collision)
    {
        if (_hasOnCollisionEnter)
        {
            CallLuaMethod("OnCollisionEnter", collision);
        }
    }
    
    void LuaComponent::OnCollisionStay(const Physics::Collision& collision)
    {
        if (_hasOnCollisionStay)
        {
            CallLuaMethod("OnCollisionStay", collision);
        }
    }
    
    void LuaComponent::OnCollisionExit(const Physics::Collision& collision)
    {
        if (_hasOnCollisionExit)
        {
            CallLuaMethod("OnCollisionExit", collision);
        }
    }
    
    void LuaComponent::OnTriggerEnter(Physics::Trigger trigger)
    {
        if (_hasOnTriggerEnter)
        {
            CallLuaMethod("OnTriggerEnter", trigger);
        }
    }
    
    void LuaComponent::OnTriggerStay(Physics::Trigger trigger)
    {
        if (_hasOnTriggerStay)
        {
            CallLuaMethod("OnTriggerStay", trigger);
        }
    }
    
    void LuaComponent::OnTriggerExit(Physics::Trigger trigger)
    {
        if (_hasOnTriggerExit)
        {
            CallLuaMethod("OnTriggerExit", trigger);
        }
    }
    
    nlohmann::json LuaComponent::Serialize() const
    {
        auto j = SerializableComponent::Serialize();
        j["scriptPath"] = _scriptPath;
        j["scriptData"] = _scriptData;
        return j;
    }
    
    void LuaComponent::Deserialize(const nlohmann::json& j, ReferenceResolver* resolver)
    {
        SerializableComponent::Deserialize(j, resolver);
        
        if (j.contains("scriptData"))
        {
            _scriptData = j["scriptData"];
        }
        
        if (j.contains("scriptPath"))
        {
            IO::ResourcePath path = j["scriptPath"].get<IO::ResourcePath>();
            SetScript(path);
        }
    }
    
    void LuaComponent::SetScriptData(const nlohmann::json& data)
    {
        _scriptData = data;
        InjectFieldsIntoScript();
    }
    
    template<typename T>
    T LuaComponent::GetField(const std::string& fieldName, T defaultValue) const
    {
        if (_scriptData.contains(fieldName))
        {
            return _scriptData[fieldName].get<T>();
        }
        return defaultValue;
    }
    
    template<typename T>
    void LuaComponent::SetField(const std::string& fieldName, const T& value)
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
            return;
        
        auto savedData = _scriptData;
        
        InitializeScriptInstance();
        ExtractSerializableFields();
        
        _scriptData = savedData;
        InjectFieldsIntoScript();
        CacheLifecycleMethods();
        
        Logger::Info(std::format("Reloaded script: {}", _scriptPath.ToString()));
    }
    
    // Register with ComponentRegistry
    namespace
    {
        struct LuaComponentRegistrar
        {
            LuaComponentRegistrar()
            {
                ComponentRegistry::Instance().Register("LuaComponent",
                    [](GameObject& go) -> std::unique_ptr<Component>
                    {
                        return std::make_unique<LuaComponent>(go);
                    });
            }
        } g_luaComponentRegistrar;
        
        // Register script loader
        struct LuaScriptLoaderRegistrar
        {
            LuaScriptLoaderRegistrar()
            {
                IO::ResourceLoader::Instance().RegisterSimpleLoader<LuaScript>(".lua");
            }
        } g_luaScriptLoader;
    }
}
