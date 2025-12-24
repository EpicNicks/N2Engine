#pragma once

#include <sol/sol.hpp>
#include <nlohmann/json.hpp>
#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Scripting
{
    class LuaComponent : public SerializableComponent
    {
    private:
        IO::ResourcePath _scriptPath;
        LuaScript* _script = nullptr;
        sol::table _scriptInstance;
        
        nlohmann::json _scriptData;
        
        bool _hasOnUpdate = false;
        bool _hasOnFixedUpdate = false;
        bool _hasOnLateUpdate = false;
        bool _hasOnCollisionEnter = false;
        bool _hasOnCollisionStay = false;
        bool _hasOnCollisionExit = false;
        bool _hasOnTriggerEnter = false;
        bool _hasOnTriggerStay = false;
        bool _hasOnTriggerExit = false;
        
    public:
        explicit LuaComponent(GameObject& gameObject);
        
        void SetScript(const IO::ResourcePath& path);
        const IO::ResourcePath& GetScriptPath() const { return _scriptPath; }
        LuaScript* GetScript() const { return _script; }
        
        nlohmann::json GetScriptData() const { return _scriptData; }
        void SetScriptData(const nlohmann::json& data);
        
        template<typename T>
        T GetField(const std::string& fieldName, T defaultValue = T{}) const;
        
        template<typename T>
        void SetField(const std::string& fieldName, const T& value);
        
        std::string GetTypeName() const override { return "LuaComponent"; }
        
        void OnAttach() override;
        void OnUpdate() override;
        void OnFixedUpdate() override;
        void OnLateUpdate() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;
        
        void OnCollisionEnter(const Physics::Collision& collision) override;
        void OnCollisionStay(const Physics::Collision& collision) override;
        void OnCollisionExit(const Physics::Collision& collision) override;
        void OnTriggerEnter(Physics::Trigger trigger) override;
        void OnTriggerStay(Physics::Trigger trigger) override;
        void OnTriggerExit(Physics::Trigger trigger) override;
        
        nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json& j, ReferenceResolver* resolver) override;
        
        void ReloadScript();
        
    private:
        void InitializeScriptInstance();
        void CacheLifecycleMethods();
        void ExtractSerializableFields();
        void InjectFieldsIntoScript();
        
        template<typename... Args>
        void CallLuaMethod(const std::string& methodName, Args&&... args);
    };
}
