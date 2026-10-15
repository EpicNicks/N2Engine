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
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>

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
            GuardedAccess([this]
            {
                _scriptInstance["component"] = sol::lua_nil;
                _scriptInstance["gameObject"] = sol::lua_nil;
            });
        }
    }

    void LuaComponent::ReportScriptError(const std::string_view what, const std::string_view message)
    {
        if (LuaRuntime::IsLimitError(message))
        {
            if (!_faulted)
            {
                _faulted = true;
                Logger::Error(std::format("Lua {} in {} aborted: {}. The script is switched off until it is set again or reloaded.",
                                          what, _scriptPath.ToString(), message));
            }
            return;
        }
        Logger::Error(std::format("Lua {} error in {}: {}", what, _scriptPath.ToString(), message));
    }

    bool LuaComponent::GuardedAccess(const std::function<void()> &body)
    {
        // A Lua protected call, so an error in a metamethod (or the budget) is a return value here: never a panic
        // (which would leave the count hook disabled) and never a jump over this function's frames
        std::string error;
        if (LuaRuntime::Instance().ProtectedCall(body, error))
        {
            return true;
        }
        ReportScriptError("access", error);
        return false;
    }

    bool LuaComponent::HasMethod(const char *methodName)
    {
        if (!_scriptInstance.valid())
            return false;

        bool has = false;
        GuardedAccess([&] { has = _scriptInstance[methodName].valid(); });
        return has;
    }

    template <typename... Args>
    void LuaComponent::CallLuaMethod(const std::string &methodName, Args &&... args)
    {
        if (_hasMissingScript || _faulted)
            return; // Silently skip if script is missing, or it ran out of budget

        // One budget for the whole call, whatever it triggers (see LuaBudgetScope)
        const LuaBudgetScope budget;

        // Callbacks the script registers during this call (e.g. Subscribe in OnAttach) are tied to this
        // component (and to this script instance: see RetireScriptInstance)
        ScriptLifetimeScope scope(_alive);

        try
        {
            // Reading the method can run the script's __index: do it in a protected call
            sol::protected_function func;
            if (!GuardedAccess([&] { func = _scriptInstance[methodName].get<sol::protected_function>(); }))
            {
                return;
            }
            auto result = func(_scriptInstance, std::forward<Args>(args)...);

            if (!result.valid())
            {
                sol::error err = result;
                ReportScriptError(methodName, err.what());
            }
        }
        catch (const std::exception &e)
        {
            ReportScriptError(methodName, e.what());
        }
    }

    void LuaComponent::SetScript(const IO::ResourcePath &path)
    {
        // A different script keeps only the fields it declares itself (not on the first SetScript, which
        // may follow Deserialize filling _scriptData)
        const LuaBudgetScope budget;
        const bool otherScript = _scriptPath.IsValid() && _scriptPath != path;
        _scriptPath = path;
        _script = nullptr;

        // Use ResourceLoader and gracefully handle missing files
        auto scriptAsset = IO::ResourceLoader::Instance().Load<LuaScript>(path);
        if (!scriptAsset)
        {
            Logger::Warn(std::format("Script file not found or failed to load: {}", path.ToString()));
            // The previous script used to stay referenced, and its subscriptions kept firing
            RetireScriptInstance();
            _faulted = false;
            _hasMissingScript = true;
            CacheLifecycleMethods();
            return;
        }

        _script = scriptAsset;
        LoadScriptInstance(false, otherScript);

        // Register reload callback, replacing any from a previous SetScript; removed on destroy
        std::string moduleName = LuaRuntime::Instance().PathToModuleName(path);
        LuaRuntime::Instance().UnregisterReloadCallbacks(this);
        LuaRuntime::Instance().RegisterReloadCallback(moduleName, [this]()
        {
            ReloadScript();
        }, this);
    }

    void LuaComponent::ClearScript()
    {
        const LuaBudgetScope budget;
        LuaRuntime::Instance().UnregisterReloadCallbacks(this);
        RetireScriptInstance();
        _faulted = false;
        _scriptPath = IO::ResourcePath{};
        _script = nullptr;
        _hasMissingScript = false;
        CacheLifecycleMethods();
    }

    sol::table LuaComponent::CreateScriptInstance()
    {
        if (!_script)
            return sol::table{};

        auto &lua = LuaRuntime::Instance().GetState();

        // The chunk and the instance setup below (assigning into a table whose metatable the script chose can run its
        // __newindex) share one budget
        const LuaBudgetScope budget;
        auto result = LuaRuntime::Instance().RunSource(_script->GetSourceCode(), _scriptPath.ToString());

        if (!result.valid())
        {
            sol::error err = result;
            if (!LuaRuntime::IsLimitError(err.what()) || LuaRuntime::Instance().ShouldLogLimitError(_scriptPath.ToString()))
            {
                Logger::Error(std::format("Failed to load script {}: {}", _scriptPath.ToString(), err.what()));
            }
            return sol::table{};
        }

        sol::table scriptClass;

        if (result.return_count() > 0 && result[0].is<sol::table>())
        {
            scriptClass = result[0];
        }
        else
        {
            Logger::Error("Script must return a table");
            return sol::table{};
        }

        // The VM runs a __gc finalizer with its debug hooks off, so the budget could not stop a looping one
        if (const sol::type gcType = scriptClass.raw_get<sol::object>("__gc").get_type();
            gcType != sol::type::lua_nil && gcType != sol::type::none)
        {
            Logger::Error(std::format("Script {} defines __gc, which scripts may not use", _scriptPath.ToString()));
            return sol::table{};
        }

        sol::table instance = lua.create_table();
        instance[sol::metatable_key] = scriptClass;

        // Handles, so anything that copies them out of self can't reach freed objects later
        const bool bound = GuardedAccess([&]
        {
            instance["component"] = ComponentRef<LuaComponent>(*this);
            instance["gameObject"] = GameObjectRef(_gameObject);
        });
        if (!bound)
        {
            return sol::table{};
        }

        return instance;
    }

    void LuaComponent::LoadScriptInstance(const bool keepOldOnFailure, const bool keepOnlyDeclaredFields)
    {
        // The chunk, the setup that follows it and the OnAttach of a replaced instance share one budget
        const LuaBudgetScope budget;
        sol::table instance = CreateScriptInstance();
        if (!instance.valid() && keepOldOnFailure && _scriptInstance.valid() && !_hasMissingScript)
        {
            Logger::Warn(std::format("Keeping the running version of {}", _scriptPath.ToString()));
            return;
        }

        const sol::table previous = RetireScriptInstance();
        _faulted = false; // a new instance (or none) replaces the one that ran out of budget
        if (!instance.valid())
        {
            _hasMissingScript = true;
            CacheLifecycleMethods();
            return;
        }

        _scriptInstance = instance;
        _hasMissingScript = false;
        // The script's metamethods can run in every step below, outside a protected call
        const bool setUp = GuardedAccess([&]
        {
            if (keepOnlyDeclaredFields)
            {
                // An unrelated script must not inherit another's values or references under the same name
                // unless it declares that field (and then, as a field of the component, it keeps the value)
                const sol::optional<sol::table> declared = _scriptInstance["SerializableFields"];
                nlohmann::json kept = nlohmann::json::object();
                if (declared && _scriptData.is_object())
                {
                    for (auto &[fieldName, value] : _scriptData.items())
                    {
                        if (declared->get<sol::object>(fieldName).valid())
                        {
                            kept[fieldName] = value;
                        }
                    }
                }
                _scriptData = std::move(kept);
            }
            // Fields the script declares but _scriptData lacks get their defaults; saved values are kept
            ExtractSerializableFields();
            InjectFieldsIntoScript();
            CopyReferenceFields(previous);
        });
        if (!setUp)
        {
            _faulted = true; // half set up: no lifecycle calls until it is set again or reloaded
        }
        CacheLifecycleMethods();

        // Already attached: the first OnAttach won't come again, so the new instance gets its own
        if (_attached && HasMethod("OnAttach"))
        {
            CallLuaMethod("OnAttach");
        }
    }

    sol::table LuaComponent::RetireScriptInstance()
    {
        sol::table previous = _scriptInstance;
        if (!previous.valid())
        {
            return previous;
        }

        const LuaBudgetScope budget;
        // The same teardown a removed component's script gets, so it can undo what OnAttach set up
        if (_attached && !_hasMissingScript && !_faulted)
        {
            if (IsActive() && HasMethod("OnDisable"))
            {
                CallLuaMethod("OnDisable");
            }
            if (HasMethod("OnDestroy"))
            {
                CallLuaMethod("OnDestroy");
            }
        }

        // Its subscriptions stop firing; the next instance's get a flag of their own
        *_alive = false;
        _alive = std::make_shared<bool>(true);
        GuardedAccess([&]
        {
            previous["component"] = sol::lua_nil;
            previous["gameObject"] = sol::lua_nil;
        });
        _scriptInstance = sol::lua_nil;
        return previous;
    }

    void LuaComponent::CopyReferenceFields(const sol::table &from)
    {
        if (!from.valid() || !_scriptInstance.valid())
            return;

        GuardedAccess([&]
        {
        for (auto &[fieldName, value] : _scriptData.items())
        {
            if (value.is_object() && value.contains("$ref"))
            {
                _scriptInstance[fieldName] = from.get<sol::object>(fieldName);
            }
        }
        });
    }

    void LuaComponent::ExtractSerializableFields()
    {
        if (!_scriptInstance.valid())
            return;

        GuardedAccess([&]
        {
        sol::optional<sol::table> fieldsTable = _scriptInstance["SerializableFields"];
        if (!fieldsTable)
            return;

        for (const auto &[key, value] : *fieldsTable)
        {
            // A field is named by a string; any other key (a positional entry) would make sol panic in as<>
            if (key.get_type() != sol::type::string)
                continue;
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
        });
    }

    void LuaComponent::InjectFieldsIntoScript()
    {
        if (!_scriptInstance.valid())
            return;

        GuardedAccess([&]
        {
        for (auto &[key, value] : _scriptData.items())
        {
            if (value.is_number_float())
                _scriptInstance[key] = value.get<float>();
            else if (value.is_number_integer())
                _scriptInstance[key] = value.get<std::int64_t>();
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
        });
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
            _hasOnApplicationQuit = false;
            _hasOnMouseEnter = false;
            _hasOnMouseOver = false;
            _hasOnMouseExit = false;
            _hasOnMouseDown = false;
            _hasOnMouseDrag = false;
            _hasOnMouseUp = false;
            _hasOnMouseUpAsButton = false;
            return;
        }

        _hasOnUpdate = HasMethod("OnUpdate");
        _hasOnFixedUpdate = HasMethod("OnFixedUpdate");
        _hasOnLateUpdate = HasMethod("OnLateUpdate");
        _hasOnCollisionEnter = HasMethod("OnCollisionEnter");
        _hasOnCollisionStay = HasMethod("OnCollisionStay");
        _hasOnCollisionExit = HasMethod("OnCollisionExit");
        _hasOnTriggerEnter = HasMethod("OnTriggerEnter");
        _hasOnTriggerStay = HasMethod("OnTriggerStay");
        _hasOnTriggerExit = HasMethod("OnTriggerExit");
        _hasOnApplicationQuit = HasMethod("OnApplicationQuit");
        _hasOnMouseEnter = HasMethod("OnMouseEnter");
        _hasOnMouseOver = HasMethod("OnMouseOver");
        _hasOnMouseExit = HasMethod("OnMouseExit");
        _hasOnMouseDown = HasMethod("OnMouseDown");
        _hasOnMouseDrag = HasMethod("OnMouseDrag");
        _hasOnMouseUp = HasMethod("OnMouseUp");
        _hasOnMouseUpAsButton = HasMethod("OnMouseUpAsButton");
    }

    void LuaComponent::OnAttach()
    {
        _attached = true; // from now on a replaced script instance is torn down and the new one attached

        if (_hasMissingScript)
        {
            Logger::Warn(std::format("LuaComponent on '{}' has missing script: {}",
                                     _gameObject.GetName(),
                                     _scriptPath.ToString()));
            return;
        }

        if (HasMethod("OnAttach"))
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
        if (!_hasMissingScript && !_faulted && HasMethod("OnDestroy"))
        {
            CallLuaMethod("OnDestroy");
        }
        ReleaseScript();
    }

    void LuaComponent::OnEnable()
    {
        if (!_hasMissingScript && !_faulted && HasMethod("OnEnable"))
        {
            CallLuaMethod("OnEnable");
        }
    }

    void LuaComponent::OnDisable()
    {
        if (!_hasMissingScript && !_faulted && HasMethod("OnDisable"))
        {
            CallLuaMethod("OnDisable");
        }
    }

    void LuaComponent::OnApplicationQuit()
    {
        if (_hasOnApplicationQuit && !_hasMissingScript)
        {
            CallLuaMethod("OnApplicationQuit");
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

    // Pointer events carry no argument; scripts read Input.GetMousePosition if they need the cursor
    void LuaComponent::OnMouseEnter()
    {
        if (_hasOnMouseEnter && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseEnter");
        }
    }

    void LuaComponent::OnMouseOver()
    {
        if (_hasOnMouseOver && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseOver");
        }
    }

    void LuaComponent::OnMouseExit()
    {
        if (_hasOnMouseExit && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseExit");
        }
    }

    void LuaComponent::OnMouseDown()
    {
        if (_hasOnMouseDown && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseDown");
        }
    }

    void LuaComponent::OnMouseDrag()
    {
        if (_hasOnMouseDrag && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseDrag");
        }
    }

    void LuaComponent::OnMouseUp()
    {
        if (_hasOnMouseUp && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseUp");
        }
    }

    void LuaComponent::OnMouseUpAsButton()
    {
        if (_hasOnMouseUpAsButton && !_hasMissingScript)
        {
            CallLuaMethod("OnMouseUpAsButton");
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

        const LuaBudgetScope budget; // SetScript and what follows it each open one; this keeps them in the one entry
        if (j.contains("scriptData") && !j["scriptData"].is_null())
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
            if (path.IsValid())
            {
                SetScript(path);
            }
            else if (_scriptPath.IsValid() || _hasMissingScript)
            {
                // Saved with no script (a new component's scriptPath is empty): a script it has since been given goes
                ClearScript();
            }
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

        GuardedAccess([&]
        {
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
        });
    }

    bool LuaComponent::IsComponentType(const std::string &type) const
    {
        // Any type scripts can add by name (Rigidbody, BoxCollider, ...), or a name ending in "Component".
        // Only the latter used to count, so `type = "Rigidbody"` reference fields were silently skipped.
        const auto names = Bindings::GetScriptableComponentNames();
        return std::ranges::find(names, type) != names.end() ||
               (type.size() > 9 && type.ends_with("Component"));
    }

    std::vector<FieldInfo> LuaComponent::DescribeFields() const
    {
        std::vector<FieldInfo> fields;

        FieldInfo script;
        script.name = "scriptUUID";
        script.displayName = "Script";
        script.kind = FieldKind::AssetRef;
        script.typeName = std::string(LuaScript::ResourceTypeName);
        script.assetType = script.typeName;
        fields.push_back(std::move(script));

        sol::table instance = _scriptInstance;
        if (!instance.valid())
        {
            return fields;
        }
        const LuaBudgetScope budget;
        // Reading a field of the script's tables can run its metamethods: each read is a Lua protected call
        const auto guarded = [this](const std::function<void()> &body)
        {
            std::string error;
            if (LuaRuntime::Instance().ProtectedCall(body, error))
            {
                return true;
            }
            Logger::Warn(std::format("Lua error reading the fields of {}: {}", _scriptPath.ToString(), error));
            return false;
        };
        sol::optional<sol::table> declared;
        if (!guarded([&] { declared = instance["SerializableFields"].get<sol::optional<sol::table>>(); }) || !declared)
        {
            return fields;
        }

        std::vector<FieldInfo> scriptFields;
        for (const auto &[key, value] : *declared)
        {
            // A field is named by a string (a table can be keyed by anything: the script's own business)
            if (key.get_type() != sol::type::string)
            {
                continue;
            }
            FieldInfo info;
            info.name = key.as<std::string>();
            info.displayName = DefaultDisplayName(info.name);
            info.container = "scriptData";

            // Either { default = 5, type = "..." } or the shorthand `speed = 5`
            std::string declaredType;
            sol::object defaultValue = value;
            if (value.is<sol::table>())
            {
                const sol::table definition = value.as<sol::table>();
                guarded([&]
                {
                    declaredType = definition.get_or<std::string>("type", "");
                    defaultValue = definition.get<sol::object>("default");
                });
            }

            if (declaredType == "GameObject")
            {
                info.kind = FieldKind::GameObjectRef;
                info.typeName = "GameObject";
            }
            else if (!declaredType.empty() && IsComponentType(declaredType))
            {
                info.kind = FieldKind::ComponentRef;
                info.typeName = declaredType;
            }
            else if (declaredType == "string")
            {
                info.kind = FieldKind::String;
                info.typeName = "string";
            }
            else if (declaredType == "int" || declaredType == "integer")
            {
                info.kind = FieldKind::Int;
                info.typeName = "int";
            }
            else if (declaredType == "number" || declaredType == "float")
            {
                info.kind = FieldKind::Float;
                info.typeName = "float";
            }
            else if (declaredType == "bool" || declaredType == "boolean")
            {
                info.kind = FieldKind::Bool;
                info.typeName = "bool";
            }
            else if (declaredType == "Vector3")
            {
                info.kind = FieldKind::Vector3;
                info.typeName = "Vector3";
            }
            else if (defaultValue.get_type() == sol::type::number)
            {
                // As ExtractSerializableFields stores it: a Lua integer is an integer
                lua_State *L = defaultValue.lua_state();
                defaultValue.push(L);
                const bool isInteger = lua_isinteger(L, -1) != 0;
                lua_pop(L, 1);
                info.kind = isInteger ? FieldKind::Int : FieldKind::Float;
                info.typeName = isInteger ? "int" : "float";
            }
            else if (defaultValue.is<bool>())
            {
                info.kind = FieldKind::Bool;
                info.typeName = "bool";
            }
            else if (defaultValue.is<std::string>())
            {
                info.kind = FieldKind::String;
                info.typeName = "string";
            }
            else if (defaultValue.is<Math::Vector3>())
            {
                info.kind = FieldKind::Vector3;
                info.typeName = "Vector3";
            }
            // else Json (the FieldInfo's own default): whatever the data holds
            if (info.kind == FieldKind::Int)
            {
                // Stored as a JSON integer and injected as a Lua integer
                info.limits = {static_cast<double>(std::numeric_limits<std::int64_t>::lowest()),
                               static_cast<double>(std::numeric_limits<std::int64_t>::max())};
            }
            scriptFields.push_back(std::move(info));
        }

        // A Lua table's order isn't the order it was written in
        std::ranges::sort(scriptFields, {}, &FieldInfo::name);
        fields.insert(fields.end(), std::make_move_iterator(scriptFields.begin()),
                      std::make_move_iterator(scriptFields.end()));
        return fields;
    }

    void LuaComponent::SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver)
    {
        if (!values.is_object())
        {
            return;
        }

        const LuaBudgetScope budget;
        if (const auto found = values.find("scriptUUID"); found != values.end())
        {
            if (!found->is_string())
            {
                throw std::invalid_argument("A LuaComponent needs a script: scriptUUID can't be null");
            }
            const auto uuid = Math::UUID::FromString(found->get<std::string>());
            const IO::AssetMetadata *meta = uuid.has_value() ? IO::ResourceLoader::Instance().GetMetadata(uuid.value()) : nullptr;
            if (meta == nullptr)
            {
                throw std::invalid_argument(std::format("Script not found: {}", found->get<std::string>()));
            }
            if (meta->resourceType != LuaScript::ResourceTypeName)
            {
                throw std::invalid_argument(std::format("Asset {} is a {}; field 'scriptUUID' expects LuaScript",
                                                        found->get<std::string>(), meta->resourceType));
            }
            // The script it already runs: nothing to reload, unless it failed to load (choosing it again retries)
            if (meta->resourcePath != _scriptPath || _hasMissingScript)
            {
                SetScript(meta->resourcePath);
            }
        }

        if (const auto found = values.find("scriptData"); found != values.end() && found->is_object())
        {
            if (!_scriptData.is_object())
            {
                _scriptData = nlohmann::json::object();
            }
            const std::vector<FieldInfo> fields = DescribeFields();
            for (const auto &[fieldName, fieldValue] : found->items())
            {
                // An integer field is stored as an integer, whatever the client's number looked like
                const auto field = std::ranges::find_if(fields, [&fieldName](const FieldInfo &candidate)
                {
                    return candidate.container == "scriptData" && candidate.name == fieldName;
                });
                _scriptData[fieldName] = field != fields.end() ? ClampFieldValue(*field, fieldValue) : fieldValue;
            }
            InjectFieldsIntoScript();
            if (resolver != nullptr)
            {
                // References are objects of the scene: found once the caller resolves
                resolver->AddPendingReference([this, resolver]()
                {
                    ResolveReferences(_scriptData, resolver);
                });
            }
        }
    }

    void LuaComponent::SetScriptData(const nlohmann::json &data)
    {
        const LuaBudgetScope budget;
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
            GuardedAccess([&] { _scriptInstance[fieldName] = value; });
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

        // The loader's copy is the current one: a file deleted and put back is a new object, and this component's
        // own would hold the old source
        if (auto current = IO::ResourceLoader::Instance().GetCached<LuaScript>(_scriptPath))
        {
            _script = std::move(current);
        }

        // Saved fields (and resolved references) carry over, fields the new version adds get their
        // defaults, and the old instance is retired properly: see LoadScriptInstance
        LoadScriptInstance(true, false);

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
