#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;
namespace ScriptingDetail = N2Engine::Scripting::Detail;

// engine/src/scripting/LuaPanicRepair.cpp and LuaRuntime.cpp: the length of the registry's free list, the registry's
// size, and the allocator's fault hook (compiled in only with N2ENGINE_BUILD_TESTS: the engine target defines
// N2ENGINE_LUA_ALLOC_FAULT_HOOK then, and a shipped build has neither the hook nor its check in the allocator)
namespace N2Engine::Scripting::Detail
{
    int CountFreeRegistrySlots(lua_State *L, int limit);
    std::uint64_t RegistryCapacity(lua_State *L);
    void SetGrowthAllowanceForTest(lua_State *L, std::uint64_t allowed);
    std::uint64_t GrowthRequestsForTest(lua_State *L);
}

// Registry references (audit F3a follow-up). sol creates and copies a reference with luaL_ref, in a noexcept
// constructor; when the registry must grow for it at the Lua memory cap, outside a protected call, the failure used to
// end the process. Every path that makes such a reference now reserves free registry slots in a protected call first
// (LuaRuntime::ReserveReferences), or makes it inside one, or moves instead of copying. These tests put the registry at
// its edge (no free slot, the next reference needs an allocation) and make that allocation fail: at the memory cap, or
// as the Nth growth of a path (the allocator's fault hook). Nothing here may end the test binary.
class LuaRegistryReserveTest : public ::testing::Test
{
protected:
    static constexpr std::uint64_t TestBudget = 2'000'000;
    static constexpr std::size_t MiB = std::size_t{1024} * 1024;
    static constexpr std::uint64_t Unlimited = std::numeric_limits<std::uint64_t>::max();

    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_registry_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        WriteAsset("registry/Plain.lua", R"(
            local Plain = {}
            Plain.__index = Plain
            function Plain:OnAttach() registry_events = (registry_events or 0) + 1 end
            function Plain:OnDisable() registry_events = (registry_events or 0) + 1 end
            function Plain:OnDestroy() registry_events = (registry_events or 0) + 1 end
            function Plain:OnUpdate() end
            return Plain
        )");

        WriteAsset("registry/Fields.lua", R"(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                speed = 5,
                label = { type = "string", default = "x" },
                flag = { default = true },
            }
            function Fields:OnUpdate() end
            return Fields
        )");

        WriteAsset("registry/Collider.lua", R"(
            local Collider = {}
            Collider.__index = Collider
            function Collider:OnCollisionEnter(collision) registry_hits = registry_hits + 1 end
            function Collider:OnTriggerEnter(trigger) registry_hits = registry_hits + 1 end
            return Collider
        )");

        // __gc as a plain field of the class (setmetatable refuses it, the class table is the script's own)
        WriteAsset("registry/GcClass.lua", R"(
            local GcClass = {}
            GcClass.__index = GcClass
            GcClass.__gc = function() end
            return GcClass
        )");

        WriteAsset("registry/Module.lua", "return { value = 1 }\n");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static LuaRuntime &Runtime() { return LuaRuntime::Instance(); }
    static sol::state &Lua() { return Runtime().GetState(); }
    static lua_State *L() { return Lua().lua_state(); }

    LuaLimits _saved;
    std::vector<std::pair<std::string, Logger::LogLevel>> _logs;
    std::size_t _logSubscription = 0;

    void SetUp() override
    {
        _saved = Runtime().GetLimits();
        Runtime().SetLimits({TestBudget, Runtime().GetMemoryUsed() + 64 * MiB});
        _logSubscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        {
            _logs.emplace_back(std::string(message), level);
        };
        Lua()["registry_events"] = 0;
        Lua()["registry_hits"] = 0;
    }

    void TearDown() override
    {
        ScriptingDetail::SetGrowthAllowanceForTest(L(), Unlimited);
        Logger::logEvent -= _logSubscription;
        Runtime().SetLimits(_saved);
        Lua()["registry_events"] = sol::lua_nil;
        Lua()["registry_hits"] = sol::lua_nil;
    }

    int CountLogs(const std::string_view needle, const Logger::LogLevel level) const
    {
        int found = 0;
        for (const auto &[message, logLevel] : _logs)
        {
            if (logLevel == level && message.find(needle) != std::string::npos)
            {
                ++found;
            }
        }
        return found;
    }

    std::string DumpLogs() const
    {
        std::string out = "\ncaptured log lines (" + std::to_string(_logs.size()) + "):";
        for (const auto &[message, logLevel] : _logs)
        {
            const char *level = logLevel == Logger::LogLevel::Error ? "Error"
                                : logLevel == Logger::LogLevel::Warn ? "Warn"
                                                                     : "Info";
            out += "\n  [" + std::string(level) + "] " + message;
        }
        return out;
    }

    static IO::ResourcePath PathOf(const char *name) { return IO::ResourcePath(std::string("res://registry/") + name); }

    // The registry at its edge: every free slot taken, so the next reference needs the registry to allocate. The cap is
    // lowered to exactly what the state holds while the slots are taken (nothing can grow, so the loop ends where the
    // registry cannot), then put back; HoldAtCap lowers it again. The taken slots are given back when this ends.
    class RegistryEdge
    {
    public:
        RegistryEdge()
        {
            LuaRuntime &runtime = Runtime();
            _saved = runtime.GetLimits();
            std::string error;
            // The protected call's own frame exists before the cap closes in, so what fails at the cap is the registry
            EXPECT_TRUE(runtime.ProtectedCall([] {}, error)) << error;
            Lua().collect_garbage(); // an allocation that fails at the cap runs a collection and retries: nothing to free
            runtime.SetLimits({_saved.instructionsPerEntry, runtime.GetMemoryUsed()});
            lua_State *state = L();
            _refs.reserve(1024);
            // One protected call that takes references until the registry cannot grow (the error that ends it)
            runtime.ProtectedCall(
                [&]
                {
                    for (;;)
                    {
                        lua_pushboolean(state, 1);
                        _refs.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
                    }
                },
                error);
            EXPECT_NE(error.find("not enough memory"), std::string::npos) << error;
            error.clear();
            EXPECT_TRUE(runtime.ProtectedCall([] {}, error)) << "the protected call itself must still work: " << error;
            runtime.SetLimits(_saved);
        }

        ~RegistryEdge()
        {
            Runtime().SetLimits(_saved);
            lua_State *state = L();
            for (const int ref : _refs)
            {
                luaL_unref(state, LUA_REGISTRYINDEX, ref);
            }
        }

        RegistryEdge(const RegistryEdge &) = delete;
        RegistryEdge &operator=(const RegistryEdge &) = delete;

        void HoldAtCap()
        {
            Lua().collect_garbage();
            Runtime().SetLimits({_saved.instructionsPerEntry, Runtime().GetMemoryUsed()});
        }

        void ReleaseCap() { Runtime().SetLimits(_saved); }

    private:
        LuaLimits _saved;
        std::vector<int> _refs;
    };

    struct SweepResult
    {
        int runs = 0;    // times the path ran
        int refused = 0; // runs in which an allocation was refused
        int caught = 0;  // runs that ended in a C++ exception from the panic path (an allocation outside a protected call)
    };

    // Runs `act` once for each n, with the registry at its edge and the n first growth allocations of the state
    // allowed: the (n+1)th and every later one fails, as at the memory cap. n goes up from 0, so the failing allocation
    // moves through the path, the registry's growth among them. It ends at the first run in which the registry grew
    // (that run and every later one only repeat what the registry growing means; the earlier ones are the ones that
    // refuse it), or in which nothing was refused. `prepare` builds what `act` works on, outside the failing window;
    // `verify` is called after each run (with the registry given back and the allowance off) and tells it whether
    // nothing was refused. An exception from `act` is the panic path's (an allocation outside a protected call, which
    // the engine's own catches handle where they are): it is counted; a registry reference failing would not be an
    // exception at all but the end of the test binary, which is what these tests exist to rule out.
    SweepResult Sweep(const std::function<void()> &prepare, const std::function<void()> &act,
                      const std::function<void(bool completed)> &verify)
    {
        constexpr int MaxRuns = 20000;
        SweepResult result;
        lua_State *state = L();
        for (std::uint64_t allowed = 0; result.runs < MaxRuns; ++allowed)
        {
            prepare();
            bool refused = false;
            bool grew = false;
            {
                RegistryEdge edge;
                const std::uint64_t capacityBefore = ScriptingDetail::RegistryCapacity(state);
                const std::uint64_t requestsBefore = ScriptingDetail::GrowthRequestsForTest(state);
                const std::uint64_t panicsBefore = Runtime().PanicCount();
                ScriptingDetail::SetGrowthAllowanceForTest(state, allowed);
                try
                {
                    act();
                }
                catch (...)
                {
                    ++result.caught;
                    Runtime().RecoverAfterPanic(panicsBefore);
                }
                ScriptingDetail::SetGrowthAllowanceForTest(state, Unlimited);
                refused = ScriptingDetail::GrowthRequestsForTest(state) - requestsBefore > allowed;
                grew = ScriptingDetail::RegistryCapacity(state) != capacityBefore;
            }
            EXPECT_EQ(lua_gettop(state), 0) << "the stack is back at its base after the run with allowance " << allowed;
            verify(!refused);
            ++result.runs;
            if (refused)
            {
                ++result.refused;
            }
            if (!refused || grew)
            {
                return result;
            }
        }
        ADD_FAILURE() << "the sweep did not end within " << MaxRuns << " runs";
        return result;
    }
};

TEST_F(LuaRegistryReserveTest, TheRegistryEdgeMakesTheNextReferenceNeedAnAllocation)
{
    // The fixture of every test below, checked on its own: at the edge with the cap held, a reference cannot be made
    // in a protected call (the memory error), and once the cap is lifted it can (the registry grows)
    const auto takeOneReference = []
    {
        lua_pushboolean(L(), 1);
        const int ref = luaL_ref(L(), LUA_REGISTRYINDEX);
        luaL_unref(L(), LUA_REGISTRYINDEX, ref);
    };
    RegistryEdge edge;
    EXPECT_EQ(ScriptingDetail::CountFreeRegistrySlots(L(), 8), 0);
    edge.HoldAtCap();
    std::string error;
    const bool heldAtCap = Runtime().ProtectedCall(takeOneReference, error);
    EXPECT_FALSE(heldAtCap);
    EXPECT_NE(error.find("not enough memory"), std::string::npos) << error;
    edge.ReleaseCap();
    error.clear();
    const bool afterRelease = Runtime().ProtectedCall(takeOneReference, error);
    EXPECT_TRUE(afterRelease) << error;
    EXPECT_EQ(lua_gettop(L()), 0);
}

TEST_F(LuaRegistryReserveTest, ReservingFailsAsAnErrorWhenTheRegistryCannotGrowAndLeavesFreeSlotsWhenItCan)
{
    sol::table globals = Lua()["_G"];
    ASSERT_TRUE(globals.valid());
    RegistryEdge edge;
    edge.HoldAtCap();

    std::string error;
    EXPECT_FALSE(Runtime().ReserveReferences(3, error));
    EXPECT_NE(error.find("not enough memory"), std::string::npos) << error;
    EXPECT_EQ(ScriptingDetail::CountFreeRegistrySlots(L(), 8), 0) << "a failed reservation leaves what it took given back";

    edge.ReleaseCap();
    error.clear();
    ASSERT_TRUE(Runtime().ReserveReferences(3, error)) << error;
    EXPECT_EQ(ScriptingDetail::CountFreeRegistrySlots(L(), 8), 3);

    // Slots that are free need no allocation to reserve again
    const std::uint64_t requests = ScriptingDetail::GrowthRequestsForTest(L());
    EXPECT_TRUE(Runtime().ReserveReferences(3, error));
    EXPECT_EQ(ScriptingDetail::GrowthRequestsForTest(L()), requests);

    // The point of it: with the cap held again, so that no growth can succeed, three reference copies made outside a
    // protected call (sol's constructors are noexcept: a failing one would be std::terminate) take the reserved slots
    edge.HoldAtCap();
    {
        const sol::table first = globals;
        const sol::table second = globals;
        const sol::table third = globals;
        EXPECT_TRUE(first.valid());
        EXPECT_TRUE(second.valid());
        EXPECT_TRUE(third.valid());
        EXPECT_EQ(ScriptingDetail::CountFreeRegistrySlots(L(), 8), 0);
    }
    edge.ReleaseCap();
    EXPECT_EQ(lua_gettop(L()), 0);
}

TEST_F(LuaRegistryReserveTest, ACollisionOrTriggerHandlerAtTheEdgeAndTheCapIsFaultedAndLoggedNotFatal)
{
    const auto go = GameObject::Create("RegistryCollider");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(PathOf("Collider.lua"));
    ASSERT_FALSE(component->HasMissingScript()) << DumpLogs();
    const auto other = GameObject::Create("RegistryOther");
    Physics::Collision collision;
    collision.gameObject = go.get();
    collision.otherGameObject = other.get();
    Physics::Trigger trigger;
    trigger.gameObject = go.get();
    trigger.otherGameObject = other.get();

    {
        RegistryEdge edge;
        edge.HoldAtCap();
        // The argument is a sol::object made outside any protected call: its registry reference needs a slot, and none
        // can be had at the cap, so the engine reports that before building anything
        EXPECT_NO_THROW(component->OnCollisionEnter(collision));
        EXPECT_NO_THROW(component->OnTriggerEnter(trigger));
        edge.ReleaseCap();
    }
    EXPECT_EQ(Lua()["registry_hits"].get<int>(), 0);
    EXPECT_EQ(CountLogs("res://registry/Collider.lua aborted", Logger::LogLevel::Error), 1) << DumpLogs();
    EXPECT_EQ(CountLogs("not enough memory", Logger::LogLevel::Error), 1) << DumpLogs();
    EXPECT_EQ(lua_gettop(L()), 0);

    // Setting the script again clears the fault; the handlers deliver again
    component->SetScript(PathOf("Collider.lua"));
    component->OnCollisionEnter(collision);
    component->OnTriggerEnter(trigger);
    EXPECT_EQ(Lua()["registry_hits"].get<int>(), 2) << DumpLogs();
}

TEST_F(LuaRegistryReserveTest, ReadingAModuleAtTheEdgeAndTheCapAnswersNothingAndLogs)
{
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Module.lua"));
    ASSERT_NE(script, nullptr);
    const std::string moduleName = Runtime().PathToModuleName(PathOf("Module.lua"));
    {
        const sol::table loaded = Runtime().LoadScriptAsModule(PathOf("Module.lua"), script.get());
        ASSERT_TRUE(loaded.valid()) << DumpLogs();
    }

    {
        RegistryEdge edge;
        edge.HoldAtCap();
        sol::optional<sol::table> found;
        EXPECT_NO_THROW(found = Runtime().GetModule(moduleName));
        EXPECT_FALSE(found.has_value());
        edge.ReleaseCap();
    }
    EXPECT_EQ(CountLogs("could not be read", Logger::LogLevel::Warn), 1) << DumpLogs();

    const sol::optional<sol::table> found = Runtime().GetModule(moduleName);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ((*found)["value"].get<int>(), 1);
}

TEST_F(LuaRegistryReserveTest, TheEditorFieldsAtTheEdgeAndTheCapAreTheScriptFieldOnlyAndLogged)
{
    const auto go = GameObject::Create("RegistryFields");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(PathOf("Fields.lua"));
    ASSERT_FALSE(component->HasMissingScript()) << DumpLogs();

    std::vector<FieldInfo> atTheCap;
    {
        RegistryEdge edge;
        edge.HoldAtCap();
        EXPECT_NO_THROW(atTheCap = component->DescribeFields());
        edge.ReleaseCap();
    }
    EXPECT_EQ(atTheCap.size(), 1u) << "the reference to the script instance could not be copied: only scriptUUID";
    EXPECT_EQ(CountLogs("Lua error reading the fields", Logger::LogLevel::Warn), 1) << DumpLogs();

    const std::vector<FieldInfo> normal = component->DescribeFields();
    ASSERT_EQ(normal.size(), 4u) << DumpLogs();
}

TEST_F(LuaRegistryReserveTest, LoadingAScriptSurvivesTheRegistryGrowingAtAnyPointOfIt)
{
    GameObject::Ptr go;
    LuaComponent *component = nullptr;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryLoad");
            component = go->AddComponent<LuaComponent>();
        },
        [&] { component->SetScript(PathOf("Plain.lua")); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_FALSE(component->HasMissingScript());
            }
            component = nullptr;
            go.reset();
        });
    EXPECT_GT(result.refused, 0) << "the fault hook refused nothing: the sweep tested nothing";
}

TEST_F(LuaRegistryReserveTest, ReplacingAnAttachedScriptSurvivesTheRegistryGrowingAtAnyPointOfIt)
{
    // The old instance is retired (its OnDisable and OnDestroy run, then it is moved out of the component), the new one
    // takes its place and gets the old one's saved fields
    GameObject::Ptr go;
    LuaComponent *component = nullptr;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryReplace");
            component = go->AddComponent<LuaComponent>();
            component->SetScript(PathOf("Plain.lua"));
            component->OnAttach();
        },
        [&] { component->SetScript(PathOf("Fields.lua")); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_FALSE(component->HasMissingScript());
            }
            component = nullptr;
            go.reset();
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, AHotReloadSurvivesTheRegistryGrowingAtAnyPointOfIt)
{
    // The module is run again (its table is referenced and stored), then every component's reload callback loads its
    // script into a new instance
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Plain.lua"));
    ASSERT_NE(script, nullptr);
    GameObject::Ptr go;
    LuaComponent *component = nullptr;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryReload");
            component = go->AddComponent<LuaComponent>();
            component->SetScript(PathOf("Plain.lua"));
            component->OnAttach();
        },
        [&] { Runtime().ReloadModule(PathOf("Plain.lua"), script.get()); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_FALSE(component->HasMissingScript());
                EXPECT_TRUE(Runtime().GetModule(Runtime().PathToModuleName(PathOf("Plain.lua"))).has_value());
            }
            component = nullptr;
            go.reset();
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, LoadingAModuleSurvivesTheRegistryGrowingAtAnyPointOfIt)
{
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Module.lua"));
    ASSERT_NE(script, nullptr);
    sol::table loadedModule;
    const SweepResult result = Sweep(
        [&] { loadedModule = sol::table{}; },
        [&] { loadedModule = Runtime().LoadScriptAsModule(PathOf("Module.lua"), script.get()); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_TRUE(loadedModule.valid());
            }
            loadedModule = sol::table{};
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, AScriptThatDefinesGcIsRefusedAndTheRegistryGrowingAtAnyPointOfItIsSurvived)
{
    // Reading the class for __gc used to make a sol::object (a registry reference) outside a protected call whenever
    // the field was set; the script chooses the value
    GameObject::Ptr go;
    LuaComponent *component = nullptr;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryGc");
            component = go->AddComponent<LuaComponent>();
        },
        [&] { component->SetScript(PathOf("GcClass.lua")); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_TRUE(component->HasMissingScript());
            }
            component = nullptr;
            go.reset();
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, ReadingTheEditorFieldsSurvivesTheRegistryGrowingAtAnyPointOfIt)
{
    GameObject::Ptr go;
    LuaComponent *component = nullptr;
    std::vector<FieldInfo> fields;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryDescribe");
            component = go->AddComponent<LuaComponent>();
            component->SetScript(PathOf("Fields.lua"));
            fields.clear();
        },
        [&] { fields = component->DescribeFields(); },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_EQ(fields.size(), 4u);
            }
            EXPECT_LE(fields.size(), 4u);
            component = nullptr;
            go.reset();
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, CollisionAndTriggerHandlersSurviveTheRegistryGrowingAtAnyPointOfThem)
{
    GameObject::Ptr go;
    GameObject::Ptr other;
    LuaComponent *component = nullptr;
    Physics::Collision collision;
    Physics::Trigger trigger;
    const SweepResult result = Sweep(
        [&]
        {
            go = GameObject::Create("RegistryHandlers");
            other = GameObject::Create("RegistryHandlersOther");
            component = go->AddComponent<LuaComponent>();
            component->SetScript(PathOf("Collider.lua"));
            collision.gameObject = go.get();
            collision.otherGameObject = other.get();
            trigger.gameObject = go.get();
            trigger.otherGameObject = other.get();
            Lua()["registry_hits"] = 0;
        },
        [&]
        {
            component->OnCollisionEnter(collision);
            component->OnTriggerEnter(trigger);
        },
        [&](const bool completed)
        {
            if (completed)
            {
                EXPECT_EQ(Lua()["registry_hits"].get<int>(), 2);
            }
            component = nullptr;
            other.reset();
            go.reset();
        });
    EXPECT_GT(result.refused, 0);
}

TEST_F(LuaRegistryReserveTest, TheSitesStillDoTheirWorkWhenMemoryIsAvailable)
{
    // Nothing near the cap: what the paths above protect is unchanged
    const auto go = GameObject::Create("RegistryNormal");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(PathOf("Plain.lua"));
    ASSERT_FALSE(component->HasMissingScript()) << DumpLogs();
    component->OnAttach();
    EXPECT_EQ(Lua()["registry_events"].get<int>(), 1);

    // Replacing the script tears the old instance down (OnDestroy, and OnDisable if the component is active) and
    // installs the new one
    component->SetScript(PathOf("Fields.lua"));
    ASSERT_FALSE(component->HasMissingScript()) << DumpLogs();
    EXPECT_GE(Lua()["registry_events"].get<int>(), 2);
    const std::vector<FieldInfo> fields = component->DescribeFields();
    ASSERT_EQ(fields.size(), 4u);
    EXPECT_EQ(fields[0].name, "scriptUUID");
    EXPECT_EQ(fields[1].name, "flag");
    EXPECT_EQ(fields[2].name, "label");
    EXPECT_EQ(fields[3].name, "speed");

    // A hot reload loads the module again and every component of it into a new instance
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Fields.lua"));
    ASSERT_NE(script, nullptr);
    Runtime().ReloadModule(PathOf("Fields.lua"), script.get());
    EXPECT_FALSE(component->HasMissingScript()) << DumpLogs();
    EXPECT_TRUE(Runtime().GetModule(Runtime().PathToModuleName(PathOf("Fields.lua"))).has_value());
    EXPECT_EQ(component->DescribeFields().size(), 4u);

    // A script that defines __gc is still refused
    component->SetScript(PathOf("GcClass.lua"));
    EXPECT_TRUE(component->HasMissingScript());
    EXPECT_EQ(CountLogs("defines __gc, which scripts may not use", Logger::LogLevel::Error), 1) << DumpLogs();

    // The handlers get their argument
    component->SetScript(PathOf("Collider.lua"));
    ASSERT_FALSE(component->HasMissingScript()) << DumpLogs();
    const auto other = GameObject::Create("RegistryNormalOther");
    Physics::Collision collision;
    collision.gameObject = go.get();
    collision.otherGameObject = other.get();
    Physics::Trigger trigger;
    trigger.gameObject = go.get();
    trigger.otherGameObject = other.get();
    component->OnCollisionEnter(collision);
    component->OnTriggerEnter(trigger);
    EXPECT_EQ(Lua()["registry_hits"].get<int>(), 2);
    EXPECT_EQ(lua_gettop(L()), 0);
}
