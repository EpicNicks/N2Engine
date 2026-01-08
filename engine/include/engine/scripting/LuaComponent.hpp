#pragma once

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/io/ResourcePath.hpp"
#include "nlohmann/json.hpp"
#include <sol/sol.hpp>

namespace N2Engine
{
    class LuaScript;
}

namespace N2Engine::Scripting
{
    class LuaComponent : public SerializableComponent
    {
    private:
        IO::ResourcePath _scriptPath;
        LuaScript* _script = nullptr;
        sol::table _scriptInstance;
        nlohmann::json _scriptData;

        // Track missing lua script and refs
        bool _hasMissingScript = false;
        bool _hasUnresolvedReferences = false;

        // Cache lifecycle method existence for performance
        bool _hasOnUpdate = false;
        bool _hasOnFixedUpdate = false;
        bool _hasOnLateUpdate = false;
        bool _hasOnCollisionEnter = false;
        bool _hasOnCollisionStay = false;
        bool _hasOnCollisionExit = false;
        bool _hasOnTriggerEnter = false;
        bool _hasOnTriggerStay = false;
        bool _hasOnTriggerExit = false;


        void InitializeScriptInstance();
        void ExtractSerializableFields();
        void InjectFieldsIntoScript();
        void CacheLifecycleMethods();

        template<typename... Args>
        void CallLuaMethod(const std::string& methodName, Args&&... args);

    public:
        explicit LuaComponent(GameObject& gameObject);

        void SetScript(const IO::ResourcePath& path);
        bool IsComponentType(const std::string &type);
        void SetScriptData(const nlohmann::json& data);

        [[nodiscard]] const IO::ResourcePath& GetScriptPath() const;
        [[nodiscard]] const nlohmann::json& GetScriptData() const { return _scriptData; }
        [[nodiscard]] bool HasMissingScript() const;
        [[nodiscard]] bool HasUnresolvedReferences() const { return _hasUnresolvedReferences; }

        template<typename T>
        [[nodiscard]] T GetField(const std::string& fieldName, T defaultValue = T{}) const;

        template<typename T>
        void SetField(const std::string& fieldName, const T& value);

        void ReloadScript();

        // Component interface
        [[nodiscard]] std::string GetTypeName() const override { return "LuaComponent"; }

        // Lifecycle methods
        void OnAttach() override;
        void OnUpdate() override;
        void OnFixedUpdate() override;
        void OnLateUpdate() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;

        // Physics events
        void OnCollisionEnter(const Physics::Collision& collision) override;
        void OnCollisionStay(const Physics::Collision& collision) override;
        void OnCollisionExit(const Physics::Collision& collision) override;
        void OnTriggerEnter(Physics::Trigger trigger) override;
        void OnTriggerStay(Physics::Trigger trigger) override;
        void OnTriggerExit(Physics::Trigger trigger) override;

        // Serialization
        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json& j, ReferenceResolver* resolver) override;
        void ResolveReferences(const nlohmann::json &j, ReferenceResolver *resolver);
    };
}