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
        // Shared with the loader's cache: a rescan that forgets a deleted script's file frees the cache's copy, and a raw
        // pointer here would then dangle (the next reload would read freed memory)
        std::shared_ptr<LuaScript> _script;
        sol::table _scriptInstance;
        // True until this component is destroyed, or its script instance is replaced (SetScript, reload),
        // which gives the new instance a new flag. Callbacks the script registers hold it, so they stop
        // firing afterwards instead of running with freed or stale self.component/self.gameObject.
        std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
        // Set by the first OnAttach: a script instance replaced after that is torn down, and the new one
        // attached, as if the component had been removed and re-added
        bool _attached = false;
        // An object (empty until a script declares fields), so what GetComponent returns can be sent back
        nlohmann::json _scriptData = nlohmann::json::object();

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
        bool _hasOnMouseEnter = false;
        bool _hasOnMouseOver = false;
        bool _hasOnMouseExit = false;
        bool _hasOnMouseDown = false;
        bool _hasOnMouseDrag = false;
        bool _hasOnMouseUp = false;
        bool _hasOnMouseUpAsButton = false;

        /// Runs the script file and returns a new instance bound to this component (invalid, with the error
        /// logged, if the file fails to load or doesn't return a table)
        sol::table CreateScriptInstance();
        /// Swaps in a new instance of the current script. The old one is retired (RetireScriptInstance); the
        /// new one gets the serialized fields (defaults for fields it adds), keeps the old one's resolved
        /// $ref fields, and gets OnAttach if the component is attached. keepOldOnFailure: a new version that
        /// fails to load leaves the old instance running (hot reload) instead of a missing script.
        /// keepOnlyDeclaredFields (a different script): drop saved fields and references the new script
        /// doesn't declare in SerializableFields.
        void LoadScriptInstance(bool keepOldOnFailure, bool keepOnlyDeclaredFields);
        /// Ends the current instance as if its component were removed: OnDisable (if enabled in an active
        /// hierarchy) and OnDestroy once the component is attached, then its subscriptions stop firing and
        /// its self.component/self.gameObject are cleared. Returns it (invalid if there was none).
        sol::table RetireScriptInstance();
        /// Back to a component with no script (what a new one is): the instance is retired and the path forgotten.
        /// Deserialize does it for a saved "scriptPath" that is empty, so a component whose script was chosen in the
        /// editor can be put back as it was (undo), not left with the script or with the path "res://".
        void ClearScript();
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
        bool IsComponentType(const std::string &type) const;
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

        // Pointer events (PointerDispatcher)
        void OnMouseEnter() override;
        void OnMouseOver() override;
        void OnMouseExit() override;
        void OnMouseDown() override;
        void OnMouseDrag() override;
        void OnMouseUp() override;
        void OnMouseUpAsButton() override;

        // Serialization
        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json& j, ReferenceResolver* resolver) override;
        void ResolveReferences(const nlohmann::json &j, ReferenceResolver *resolver);

        // Editor reflection. The fields are the script (scriptUUID, an asset of type LuaScript) and one per entry of
        // the current script's SerializableFields, inside "scriptData" (their container). A type is a declared
        // type = "..." or what the default's type says; a reference is {"$ref": uuid or null}.
        [[nodiscard]] std::vector<FieldInfo> DescribeFields() const override;
        /// scriptUUID loads that script (std::invalid_argument for null, an unknown UUID or an asset that isn't a
        /// script); scriptData's keys are merged into the script's data and injected, references re-resolved
        void SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver) override;
    };
}