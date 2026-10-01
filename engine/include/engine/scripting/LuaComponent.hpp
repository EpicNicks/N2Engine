#pragma once

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/io/ResourcePath.hpp"
#include "nlohmann/json.hpp"
#include <memory>
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
        // True until this component is destroyed, or its script instance is replaced (SetScript, reload),
        // which gives the new instance a new flag. Callbacks the script registers hold it, so they stop
        // firing afterwards instead of running with freed or stale self.component/self.gameObject.
        std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
        // Set by the first OnAttach: a script instance replaced after that is torn down, and the new one
        // attached, as if the component had been removed and re-added
        bool _attached = false;
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
        bool _hasOnApplicationQuit = false;

        /// Runs the script file and returns a new instance bound to this component (invalid, with the error
        /// logged, if the file fails to load or doesn't return a table)
        sol::table CreateScriptInstance();
        /// Swaps in a new instance of the current script. The old one is retired (RetireScriptInstance); the
        /// new one gets the serialized fields (defaults for fields it adds), keeps the old one's resolved
        /// $ref fields, and gets OnAttach if the component is attached. keepOldOnFailure: a new version that
        /// fails to load leaves the old instance running (hot reload) instead of a missing script.
        void LoadScriptInstance(bool keepOldOnFailure);
        /// Ends the current instance as if its component were removed: OnDisable (if enabled in an active
        /// hierarchy) and OnDestroy once the component is attached, then its subscriptions stop firing and
        /// its self.component/self.gameObject are cleared. Returns it (invalid if there was none).
        sol::table RetireScriptInstance();
        /// $ref fields hold resolved handles on the instance, not values in _scriptData
        void CopyReferenceFields(const sol::table &from);
        void ExtractSerializableFields();
        void InjectFieldsIntoScript();
        void CacheLifecycleMethods();
        /// Cuts the script off from this component: flags its callbacks dead, clears
        /// self.component/self.gameObject and drops the reload callback. Safe to call twice.
        void ReleaseScript();

        template<typename... Args>
        void CallLuaMethod(const std::string& methodName, Args&&... args);

    public:
        explicit LuaComponent(GameObject& gameObject);
        ~LuaComponent() override;

        /// Loads the script into a new instance. Replacing an instance (a second SetScript, or ReloadScript)
        /// retires the old one first; on an attached component the new one gets OnAttach straight away
        /// (not OnEnable, as on a first attach).
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

        /// Re-runs the current script file into a new instance, like SetScript with the same path, except
        /// that a version that fails to load leaves the running instance in place
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
        void OnApplicationQuit() override;

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