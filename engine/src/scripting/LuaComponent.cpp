#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaFieldDeclaration.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/scripting/DeclarationEvaluator.hpp"
#include "engine/scripting/ScriptDeclarationCache.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/JsonLimits.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace N2Engine::Scripting
{
    namespace
    {
        /// LuaBudgetScope where scripts run; nothing where they do not (the scope would make the engine's Lua state just to
        /// count in it). The policy is set once at start-up, so the scope is opened and closed under the same one.
        class EntryScope
        {
            std::optional<LuaBudgetScope> _scope;

        public:
            EntryScope()
            {
                if (LuaRuntime::RunsScripts())
                {
                    _scope.emplace();
                }
            }
        };

        /// The list of fields is known: the script returned a table with a SerializableFields table, and nothing in the
        /// list came from the engine. (A field whose own default is unknown is still listed: that is a field, not the list.)
        bool IsReadable(const DeclarationResult &declaration)
        {
            return declaration.declared && !declaration.unevaluable;
        }

        /// What kind of default a stored value looks like, for a script whose declarations could not be read: as
        /// InjectFieldsIntoScript would hand it to the script
        LuaDefaultKind KindOfStoredValue(const nlohmann::json &value)
        {
            if (value.is_number_float())
                return LuaDefaultKind::Float;
            if (value.is_number_integer())
                return LuaDefaultKind::Integer;
            if (value.is_boolean())
                return LuaDefaultKind::Bool;
            if (value.is_string())
                return LuaDefaultKind::String;
            if (value.is_object() && value.contains("x"))
                return LuaDefaultKind::Vector3;
            return LuaDefaultKind::Other;
        }
    }

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
        UnsubscribeReload();

        // Closures the script registered can outlive this component (they keep `self` alive);
        // make those fields nil so a stray call errors in Lua instead of touching freed memory
        if (_scriptInstance.valid())
        {
            // The destructor calls this: nothing may leave it (GuardedAccess does not throw, but this is the one
            // place where an exception would be std::terminate)
            const std::uint64_t panicsBefore = LuaRuntime::Instance().PanicCount();
            try
            {
                GuardedAccess([this]
                {
                    _scriptInstance["component"] = sol::lua_nil;
                    _scriptInstance["gameObject"] = sol::lua_nil;
                }, 2);
            }
            catch (...)
            {
                LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            }
        }
    }

    void LuaComponent::UnsubscribeReload()
    {
        ScriptDeclarationCache::Instance().Unsubscribe(this);
        // Only an existing runtime has callbacks to remove: asking for it would make a Lua state (and a destructor of a
        // component on a host that runs no scripts must not)
        if (LuaRuntime::HasInstance())
        {
            LuaRuntime::Instance().UnregisterReloadCallbacks(this);
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

    bool LuaComponent::GuardedAccess(const std::function<void()> &body, const int references)
    {
        // A Lua protected call, so an error in a metamethod (or the budget) is a return value here: never a panic
        // (a C++ exception thrown through Lua's C frames) and never a jump over this function's frames
        std::string error;
        const std::uint64_t panicsBefore = LuaRuntime::Instance().PanicCount();
        try
        {
            // The bodies make sol references (a method read as a function, the fields' tables and values): free registry
            // slots first, so none of them has to grow the registry. Where Lua's errors are C++ exceptions (the web
            // build, MSVC Debug) a failing luaL_ref inside sol's noexcept constructors ends the process even in a
            // protected call; with the slots reserved it cannot fail. A body holds a few references at a time.
            if (LuaRuntime::Instance().ReserveReferences(references, error) && LuaRuntime::Instance().ProtectedCall(body, error))
            {
                return true;
            }
        }
        catch (...)
        {
            // ProtectedCall is not expected to throw (it reads its error without converting it); kept so a Lua
            // failure can never leave a lifecycle call or a destructor
            LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            error = "unknown error";
        }
        ReportScriptError("access", error);
        return false;
    }

    bool LuaComponent::HasMethod(const char *methodName)
    {
        // No instance exists where scripts do not run; if one was made under another policy it is not asked either: reading
        // a method can run the script's __index
        if (!_scriptInstance.valid() || !LuaRuntime::RunsScripts())
            return false;

        bool has = false;
        GuardedAccess([&] { has = _scriptInstance[methodName].valid(); }, 2);
        return has;
    }

    template <typename... Args>
    void LuaComponent::CallLuaMethod(const std::string &methodName, Args &&... args)
    {
        if (_hasMissingScript || _faulted)
            return; // Silently skip if script is missing, or it ran out of budget
        if (!LuaRuntime::RunsScripts())
            return; // DeclarationsOnly: there is no instance and nothing may run (no caller gets here; this is the guard)

        // One budget for the whole call, whatever it triggers (see LuaBudgetScope)
        const LuaBudgetScope budget;

        // Callbacks the script registers during this call (e.g. Subscribe in OnAttach) are tied to this
        // component (and to this script instance: see RetireScriptInstance)
        ScriptLifetimeScope scope(_alive);

        const std::uint64_t panicsBefore = LuaRuntime::Instance().PanicCount();
        try
        {
            // Reading the method can run the script's __index: do it in a protected call
            sol::protected_function func;
            if (!GuardedAccess([&] { func = _scriptInstance[methodName].get<sol::protected_function>(); }, 2))
            {
                return;
            }
            auto result = func(_scriptInstance, std::forward<Args>(args)...);

            if (!result.valid())
            {
                // The budget error from the runtime's state when it is spent (the scope above is still open), a
                // memory error from the call's status, not from an error object that can be displaced, and the text
                // read without converting (ErrorText)
                ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError(
                                                  LuaRuntime::ErrorText(result),
                                                  result.status() == sol::call_status::memory));
            }
        }
        catch (const std::exception &e)
        {
            // Includes the panic path: pushing the arguments happens before sol's protected call, so at the memory
            // cap it fails outside it and sol throws
            LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError(e.what()));
        }
        catch (...)
        {
            LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError("unknown error"));
        }
    }

    template <typename MakeArgument>
    void LuaComponent::CallLuaMethodWithArgument(const std::string &methodName, MakeArgument makeArgument)
    {
        if (_hasMissingScript || _faulted || !LuaRuntime::RunsScripts())
            return;

        // The object makeArgument returns is a registry reference made outside a protected call (sol::make_object):
        // free slots first (one is needed, a second is margin for a temporary), so it allocates nothing there (the
        // userdata it wraps still can fail: caught below)
        std::string reserveError;
        if (!LuaRuntime::Instance().ReserveReferences(2, reserveError))
        {
            ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError(reserveError));
            return;
        }
        const std::uint64_t panicsBefore = LuaRuntime::Instance().PanicCount();
        sol::object argument;
        try
        {
            argument = makeArgument();
        }
        catch (const std::exception &e)
        {
            LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError(e.what()));
            return;
        }
        catch (...)
        {
            LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
            ReportScriptError(methodName, LuaRuntime::Instance().LimitAwareError("unknown error"));
            return;
        }
        CallLuaMethod(methodName, std::move(argument));
    }

    void LuaComponent::SetScript(const IO::ResourcePath &path)
    {
        // A different script keeps only the fields it declares itself (not on the first SetScript, which
        // may follow Deserialize filling _scriptData)
        const EntryScope budget;
        const bool otherScript = _scriptPath.IsValid() && _scriptPath != path;
        _scriptPath = path;
        _script = nullptr;
        _declaration = nullptr;

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
        const std::string moduleName = LuaRuntime::PathToModuleName(path);
        // Register the reload callback, replacing any from a previous SetScript; removed on destroy
        if (!LuaRuntime::RunsScripts())
        {
            UnsubscribeReload();
            LoadDeclaration(false, otherScript);
            ScriptDeclarationCache::Instance().Subscribe(moduleName, [this]() { ReloadScript(); }, this);
            return;
        }
        LoadScriptInstance(false, otherScript);

        UnsubscribeReload();
        LuaRuntime::Instance().RegisterReloadCallback(moduleName, [this]()
        {
            ReloadScript();
        }, this);
    }

    void LuaComponent::ClearScript()
    {
        const EntryScope budget;
        UnsubscribeReload();
        RetireScriptInstance();
        _faulted = false;
        _scriptPath = IO::ResourcePath{};
        _script = nullptr;
        _declaration = nullptr;
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
        // At most the chunk's first value stays on the stack (RunChunk), so the copies below, which allocate outside
        // a protected call, cannot leave more on the stack than the panic path's padding covers
        // A failure that is the memory cap's (or the budget's) is logged once per script, whatever the step
        const auto logLoadFailure = [this](const std::string &error)
        {
            if (!LuaRuntime::IsLimitError(error) || LuaRuntime::Instance().ShouldLogLimitError(_scriptPath.ToString()))
            {
                Logger::Error(std::format("Failed to load script {}: {}", _scriptPath.ToString(), error));
            }
        };
        auto result = LuaRuntime::Instance().RunChunk(_script->GetSourceCode(), _scriptPath.ToString());

        if (!result.valid())
        {
            logLoadFailure(LuaRuntime::Instance().LimitAwareError(
                LuaRuntime::ErrorText(result), result.status() == sol::call_status::memory));
            return sol::table{};
        }

        sol::table scriptClass;

        if (result.return_count() > 0 && result[0].is<sol::table>())
        {
            // The reference to the chunk's table is made outside a protected call (a stack value to a registry
            // reference): free registry slots first (one is needed for it, one for the instance table the protected call
            // below makes, and as many again as margin for temporaries), so at the memory cap it allocates nothing
            // (sol's constructors are noexcept: a failure there would end the process)
            std::string reserveError;
            if (!LuaRuntime::Instance().ReserveReferences(4, reserveError))
            {
                logLoadFailure(LuaRuntime::Instance().LimitAwareError(reserveError));
                return sol::table{};
            }
            scriptClass = result[0];
        }
        else
        {
            Logger::Error("Script must return a table");
            return sol::table{};
        }

        // The VM runs a __gc finalizer with its debug hooks off, so the budget could not stop a looping one: the class
        // is read for it (raw, with Lua's own API: no sol reference is made for the value, which is the script's) and
        // the table and the handles are made, all in a protected call: each allocates, and at the memory cap an
        // allocation outside one would go down the panic path. The table and the handles are made so anything that
        // copies them out of self can't reach freed objects later.
        sol::table instance;
        bool definesFinalizer = false;
        const bool bound = GuardedAccess([&]
        {
            lua_State *L = scriptClass.lua_state();
            scriptClass.push(L);
            lua_pushstring(L, "__gc");
            lua_rawget(L, -2);
            const int gcType = lua_type(L, -1);
            lua_pop(L, 2);
            if (gcType != LUA_TNIL && gcType != LUA_TNONE)
            {
                definesFinalizer = true;
                return;
            }
            instance = lua.create_table();
            instance[sol::metatable_key] = scriptClass;
            instance["component"] = ComponentRef<LuaComponent>(*this);
            instance["gameObject"] = GameObjectRef(_gameObject);
        });
        if (definesFinalizer)
        {
            Logger::Error(std::format("Script {} defines __gc, which scripts may not use", _scriptPath.ToString()));
            return sol::table{};
        }
        if (!bound)
        {
            return sol::table{};
        }

        return instance;
    }

    void LuaComponent::LoadDeclaration(const bool keepOldOnFailure, const bool keepOnlyDeclaredFields)
    {
        // Evaluated off this thread, in a state of its own that has no engine binding; kept per source hash, so the same
        // text (a scene loaded twice, an undo) is read once. Never null.
        const ScriptDeclarationCache::Declaration declaration =
            ScriptDeclarationCache::Instance().Get(_scriptPath, _script->GetSourceCode());
        if (!IsReadable(*declaration) && keepOldOnFailure && _declaration != nullptr && IsReadable(*_declaration) &&
            !_hasMissingScript)
        {
            Logger::Warn(std::format("Keeping the declarations of the running version of {}", _scriptPath.ToString()));
            return;
        }

        _declaration = declaration;
        _faulted = false;
        // As a script that fails to run is: the editor shows it as one it could not load
        _hasMissingScript = !IsReadable(*_declaration);
        if (!IsReadable(*_declaration))
        {
            // _scriptData is left exactly as it is, and the step that drops what the script does not declare is skipped:
            // there is no list to keep to (the cache's log line says why)
            return;
        }

        if (!_scriptData.is_object())
        {
            _scriptData = nlohmann::json::object();
        }
        // A list that is partial (an error was reported: more fields than the limit, say) cannot say what is NOT declared
        if (keepOnlyDeclaredFields && _declaration->errors.empty())
        {
            // An unrelated script must not inherit another's values or references under the same name unless it declares
            // that field (and then, as a field of the component, it keeps the value)
            nlohmann::json kept = nlohmann::json::object();
            for (auto &[fieldName, value] : _scriptData.items())
            {
                const bool isDeclared = std::ranges::any_of(
                    _declaration->fields, [&fieldName](const DeclaredField &field) { return field.name == fieldName; });
                if (isDeclared)
                {
                    kept[fieldName] = JsonLimits::IterativeCopy(value); // field data can nest deep
                }
            }
            _scriptData = std::move(kept);
        }
        // Fields the script declares but _scriptData lacks get their defaults; saved values are kept
        for (const DeclaredField &field : _declaration->fields)
        {
            if (!_scriptData.contains(field.name) && !field.defaultValue.is_null())
            {
                _scriptData[field.name] = field.defaultValue;
            }
        }
    }

    void LuaComponent::AppendDeclaredFields(std::vector<FieldInfo> &fields) const
    {
        if (_declaration == nullptr)
        {
            return;
        }
        if (IsReadable(*_declaration))
        {
            // The same mapping as a running instance's (MakeLuaFieldInfo), sorted by name
            std::vector<FieldInfo> declared = _declaration->ToFieldInfos();
            fields.insert(fields.end(), std::make_move_iterator(declared.begin()), std::make_move_iterator(declared.end()));
            return;
        }
        // The script could not be read: nothing is lost or hidden. Every key of the data is a field, of the kind its value
        // has (an object's keys come out sorted).
        if (_scriptData.is_object())
        {
            for (const auto &[fieldName, value] : _scriptData.items())
            {
                fields.push_back(MakeLuaFieldInfo(fieldName, std::string{}, KindOfStoredValue(value)));
            }
        }
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

        // Moved, not copied: a copy is a registry reference made outside a protected call (see ReserveReferences)
        _scriptInstance = std::move(instance);
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
                            kept[fieldName] = JsonLimits::IterativeCopy(value); // field data can nest deep
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
        if (!_scriptInstance.valid())
        {
            return sol::table{};
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
            _scriptInstance["component"] = sol::lua_nil;
            _scriptInstance["gameObject"] = sol::lua_nil;
        }, 2);
        // The instance leaves the component by a move: a copy would be a registry reference made outside a protected
        // call (see ReserveReferences), and the member is empty afterwards, as `= sol::lua_nil` left it
        return std::move(_scriptInstance);
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
            CallLuaMethodWithArgument("OnCollisionEnter", [&] { return Bindings::CollisionToLua(collision, _scriptInstance.lua_state()); });
        }
    }

    void LuaComponent::OnCollisionStay(const Physics::Collision &collision)
    {
        if (_hasOnCollisionStay && !_hasMissingScript)
        {
            CallLuaMethodWithArgument("OnCollisionStay", [&] { return Bindings::CollisionToLua(collision, _scriptInstance.lua_state()); });
        }
    }

    void LuaComponent::OnCollisionExit(const Physics::Collision &collision)
    {
        if (_hasOnCollisionExit && !_hasMissingScript)
        {
            CallLuaMethodWithArgument("OnCollisionExit", [&] { return Bindings::CollisionToLua(collision, _scriptInstance.lua_state()); });
        }
    }

    void LuaComponent::OnTriggerEnter(Physics::Trigger trigger)
    {
        if (_hasOnTriggerEnter && !_hasMissingScript)
        {
            CallLuaMethodWithArgument("OnTriggerEnter", [&] { return Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()); });
        }
    }

    void LuaComponent::OnTriggerStay(Physics::Trigger trigger)
    {
        if (_hasOnTriggerStay && !_hasMissingScript)
        {
            CallLuaMethodWithArgument("OnTriggerStay", [&] { return Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()); });
        }
    }

    void LuaComponent::OnTriggerExit(Physics::Trigger trigger)
    {
        if (_hasOnTriggerExit && !_hasMissingScript)
        {
            CallLuaMethodWithArgument("OnTriggerExit", [&] { return Bindings::TriggerToLua(trigger, _scriptInstance.lua_state()); });
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

        // Copied without recursion: a script's data is scene data, which can nest up to JsonLimits::MaxSceneJsonDepth
        j["scriptData"] = JsonLimits::IterativeCopy(_scriptData);
        return j;
    }

    void LuaComponent::Deserialize(const nlohmann::json &j, ReferenceResolver *resolver)
    {
        SerializableComponent::Deserialize(j, resolver);

        const EntryScope budget; // SetScript and what follows it each open one; this keeps them in the one entry
        if (j.contains("scriptData") && !j["scriptData"].is_null())
        {
            _scriptData = JsonLimits::IterativeCopy(j["scriptData"]); // as deep as the scene allows
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
            // Not a copy of j: ResolveReferences reads _scriptData, and copying j would recurse once per JSON level
            resolver->AddPendingReference([this, resolver]()
            {
                ResolveReferences(_scriptData, resolver);
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
                const auto &refValue = value["$ref"];
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
        return IsLuaComponentType(type);
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

        if (!LuaRuntime::RunsScripts())
        {
            AppendDeclaredFields(fields);
            return fields;
        }
        if (!_scriptInstance.valid())
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
        // The declarations are read with Lua's own API into plain C++ data: a sol reference made for each key, value or
        // table would be a registry reference (luaL_ref), and where Lua's errors are C++ exceptions (the web build, MSVC
        // Debug) one that fails at the memory cap ends the process even inside a protected call, as outside one. The
        // instance is pushed on the stack, which keeps it alive while the reads run its metamethods (a SetScript from
        // one of them cannot free it).
        using DefaultKind = LuaDefaultKind;
        struct Declaration
        {
            std::string name;
            bool isDefinition = false; // { default = 5, type = "..." } rather than the shorthand `speed = 5`
            std::string declaredType;
            DefaultKind defaultKind = DefaultKind::Other; // what the default (or the shorthand value) is
        };
        const auto classify = [](lua_State *state, const int index)
        {
            switch (lua_type(state, index))
            {
            case LUA_TNUMBER:
                return lua_isinteger(state, index) != 0 ? DefaultKind::Integer : DefaultKind::Float;
            case LUA_TBOOLEAN:
                return DefaultKind::Bool;
            case LUA_TSTRING:
                return DefaultKind::String;
            default:
                return sol::stack_object(state, index).is<Math::Vector3>() ? DefaultKind::Vector3
                                                                            : DefaultKind::Other;
            }
        };

        // The state is read once, before any script code runs: a metamethod can retire the instance (SetScript), which
        // empties the member and nulls its state
        lua_State *const luaState = _scriptInstance.lua_state();
        std::vector<Declaration> declarations;
        bool declared = false;
        if (!guarded([&]
        {
            lua_State *state = luaState;
            const int base = lua_gettop(state);
            _scriptInstance.push(state);
            lua_getfield(state, -1, "SerializableFields");
            if (lua_type(state, -1) == LUA_TTABLE)
            {
                declared = true;
                const int table = lua_gettop(state);
                lua_pushnil(state);
                while (lua_next(state, table) != 0)
                {
                    // A field is named by a string (a table can be keyed by anything: the script's own business)
                    if (lua_type(state, -2) == LUA_TSTRING)
                    {
                        Declaration declaration;
                        std::size_t length = 0;
                        const char *text = lua_tolstring(state, -2, &length);
                        declaration.name.assign(text, length);
                        // As ExtractSerializableFields reads it: sol's table check accepts a userdata too
                        declaration.isDefinition = lua_type(state, -1) == LUA_TTABLE || lua_type(state, -1) == LUA_TUSERDATA;
                        if (!declaration.isDefinition)
                        {
                            declaration.defaultKind = classify(state, -1);
                        }
                        declarations.push_back(std::move(declaration));
                    }
                    lua_pop(state, 1);
                }
            }
            lua_settop(state, base);
        }) || !declared)
        {
            return fields;
        }

        // A definition's type and default are read one definition at a time, each in a protected call of its own (they
        // can run the definition's metamethods): one that fails leaves that field to its default, the others unaffected
        for (Declaration &declaration : declarations)
        {
            if (!declaration.isDefinition)
            {
                continue;
            }
            guarded([&]
            {
                // An earlier definition's metamethod may have retired the instance: nothing left to read
                if (!_scriptInstance.valid())
                {
                    return;
                }
                lua_State *state = luaState;
                const int base = lua_gettop(state);
                _scriptInstance.push(state);
                lua_getfield(state, -1, "SerializableFields");
                if (lua_type(state, -1) == LUA_TTABLE)
                {
                    lua_pushlstring(state, declaration.name.data(), declaration.name.size());
                    lua_rawget(state, -2);
                    if (lua_type(state, -1) == LUA_TTABLE || lua_type(state, -1) == LUA_TUSERDATA)
                    {
                        const int definition = lua_gettop(state);
                        lua_getfield(state, definition, "type");
                        if (lua_type(state, -1) == LUA_TSTRING)
                        {
                            std::size_t length = 0;
                            const char *text = lua_tolstring(state, -1, &length);
                            declaration.declaredType.assign(text, length);
                        }
                        lua_pop(state, 1);
                        lua_getfield(state, definition, "default");
                        declaration.defaultKind = classify(state, -1);
                    }
                }
                lua_settop(state, base);
            });
        }

        std::vector<FieldInfo> scriptFields;
        for (const Declaration &declaration : declarations)
        {
            // The mapping from a declaration to the editor field is shared with the DeclarationEvaluator
            FieldInfo info = MakeLuaFieldInfo(declaration.name, declaration.declaredType, declaration.defaultKind);
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

        const EntryScope budget;
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
                _scriptData[fieldName] = field != fields.end() ? ClampFieldValue(*field, fieldValue)
                                                               : JsonLimits::IterativeCopy(fieldValue);
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
        const EntryScope budget;
        _scriptData = JsonLimits::IterativeCopy(data);
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
        if (!LuaRuntime::RunsScripts())
        {
            LoadDeclaration(true, false);
        }
        else
        {
            LoadScriptInstance(true, false);
        }

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
