#pragma once

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/io/ResourcePath.hpp"
#include "nlohmann/json.hpp"
#include <functional>
#include <memory>
#include <string_view>
#include <sol/sol.hpp>

namespace N2Engine
{
    class LuaScript;
}

namespace N2Engine::Scripting
{
    struct DeclarationResult;

    class LuaComponent : public SerializableComponent
    {
    private:
        IO::ResourcePath _scriptPath;
        // Shared with the loader's cache: a rescan that forgets a deleted script's file frees the cache's copy, and a raw
        // pointer here would then dangle (the next reload would read freed memory)
        std::shared_ptr<LuaScript> _script;
        sol::table _scriptInstance;
        // DeclarationsOnly (LuaRuntime::GetExecutionPolicy): what the script declares, from the ScriptDeclarationCache. There
        // is no _scriptInstance then (no Lua of the project runs): this is the component's whole knowledge of its script.
        // Null when no script is set or its file is missing. A declaration that failed (declared false) is kept too, so
        // DescribeFields can say the script could not be read and still list the keys of _scriptData.
        std::shared_ptr<const DeclarationResult> _declaration;
        // DeclarationsOnly: the last reading was refused by the cache (no worker free, the command's evaluation budget
        // spent, shutdown, a thread that could not start). That is not the script's answer, so it is not kept as one: the
        // component is read again, with the same arguments, the next time it is described (RetryRefusedDeclaration).
        bool _declarationRefused = false;
        bool _refusedKeepOld = false;
        bool _refusedKeepOnlyDeclared = false;
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
        // DescribeFields said once that a declaration's `replicated` was refused (not a boolean, or a kind that cannot be
        // replicated), so describing the fields again does not repeat it
        mutable bool _replicatedWarned = false;
        // Set when a call into the script ran out of instructions or memory (LuaLimits): its lifecycle calls are skipped
        // until the script is set again or reloaded, so a looping OnUpdate doesn't burn the whole budget every frame
        bool _faulted = false;

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
        /// LoadScriptInstance for DeclarationsOnly: reads the script's declarations from the cache (evaluated off the
        /// calling thread, never in the engine's Lua state) and gives _scriptData the defaults of the fields it lacks.
        /// keepOldOnFailure: a new version whose declarations cannot be read leaves the previous ones. A script whose
        /// declarations cannot be read keeps _scriptData as it is (keepOnlyDeclaredFields is then skipped, as there is
        /// no list to keep to), and DescribeFields shows its keys.
        void LoadDeclaration(bool keepOldOnFailure, bool keepOnlyDeclaredFields);
        /// A reading the cache refused is tried again (LoadDeclaration with the refused call's arguments); a no-op
        /// otherwise. Const because the readers that need it (DescribeFields, HasMissingScript) are: a component is never
        /// a const object, and only the reading the refusal left undone changes.
        void RetryRefusedDeclaration() const;
        /// The fields DescribeFields adds after scriptUUID in DeclarationsOnly
        void AppendDeclaredFields(std::vector<FieldInfo> &fields) const;
        /// Drops this component's reload subscriptions, in the cache and (if a runtime exists) in the runtime
        void UnsubscribeReload();
        /// Ends the current instance as if its component were removed: OnDisable (if enabled in an active
        /// hierarchy) and OnDestroy once the component is attached, then its subscriptions stop firing and
        /// its self.component/self.gameObject are cleared. Returns it (invalid if there was none): moved out of the
        /// component, never copied (a reference copy outside a protected call can end the process at the memory cap).
        sol::table RetireScriptInstance();
        /// Back to a component with no script (what a new one is): the instance is retired and the path forgotten.
        /// Deserialize does it for a saved "scriptPath" that is empty, so a component whose script was chosen in the
        /// editor can be put back as it was (undo), not left with the script or with the path "res://".
        void ClearScript();
        /// $ref fields hold resolved handles on the instance, not values in _scriptData
        void CopyReferenceFields(const sol::table &from);
        void ExtractSerializableFields();
        void InjectFieldsIntoScript();
        /// InjectFieldsIntoScript for one field: the value of `key` in _scriptData is already set
        void InjectField(const std::string &key, const nlohmann::json &value);
        void CacheLifecycleMethods();
        /// Cuts the script off from this component: flags its callbacks dead, clears
        /// self.component/self.gameObject and drops the reload callback. Safe to call twice.
        void ReleaseScript();

        template<typename... Args>
        void CallLuaMethod(const std::string& methodName, Args&&... args);
        /// CallLuaMethod with one argument `makeArgument()` builds in the state (a userdata). Building it allocates
        /// outside any protected call, so at the memory cap it fails down the panic path as a C++ exception: that is
        /// caught and reported here (the component is faulted) instead of leaving through the physics callback. The
        /// sol::object it returns is a registry reference, made outside a protected call too: a free registry slot is
        /// reserved before it (LuaRuntime::ReserveReferences), and a failure to reserve is reported the same way.
        template<typename MakeArgument>
        void CallLuaMethodWithArgument(const std::string& methodName, MakeArgument makeArgument);
        /// True if the instance has that method. Reading it can run the script's __index, so it is an entry into Lua:
        /// a budget scope, and an error (or the budget) is logged and answers false.
        bool HasMethod(const char* methodName);
        /// Runs code that touches the instance outside a protected call (a table access can reach the script's
        /// metamethods) in a budget scope; an exception (a Lua error, the budget, a failed allocation) is reported and
        /// answers false
        /// `references` free registry slots are reserved first (at most 16): a body that makes only one or two
        /// references asks for few, so near the memory cap it is not refused for slots it would not use.
        bool GuardedAccess(const std::function<void()>& body, int references = 16);
        /// Logs a Lua error. One that ran out of budget or memory is logged once and faults the component.
        void ReportScriptError(std::string_view what, std::string_view message);

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

        // Network replication (netcode stage B2). A field declared `{ type = "int", default = 0, replicated = true }` is
        // replicated when it is a bool, int, float, string or Vector3; the script's LIVE value is what the server sends
        // (the instance's field, not the saved default), and a client sets only that field on its instance.
        /// Serialize() with the live values of the replicated script fields in "scriptData"
        [[nodiscard]] nlohmann::json SerializeReplicated(std::span<const FieldInfo> replicated) override;
        /// Sets the replicated script fields `values` names in "scriptData", on the instance and in the saved data; the
        /// script's other fields are not touched (SetEditorFields would write every saved field back to the instance)
        void ApplyReplicated(const nlohmann::json &values, ReferenceResolver *resolver) override;
    };
}