#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/DeclarationEvaluator.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/scripting/ScriptDeclarationCache.hpp"
#include "engine/serialization/FieldInfo.hpp"

#include "HangWatchdog.hpp"

// Lua edit-host option B, PR L2: the execution policy, a LuaComponent that reads its script's declarations from the
// ScriptDeclarationCache instead of running the script, the cache itself (content hash, off-thread evaluation, deadline,
// abandonment, worker cap, shutdown) and the Application.Quit guard.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
    // What a test thread blocks on inside the evaluator hook. Shared by the hook (which a worker may outlive the test
    // with) and the fixture, so a worker that is still waiting never touches a destroyed object.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;
        std::atomic<int> entered{0};

        void Open()
        {
            {
                const std::lock_guard lock(mutex);
                open = true;
            }
            changed.notify_all();
        }

        void Wait()
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return open; });
        }
    };

    struct Snapshot
    {
        std::vector<json> fields;
        json data;
        bool missing = false;
    };
}

class ScriptDeclarationTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << source;
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_script_declaration_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // The forms LuaReflectionTests covers, and what a real project writes around them
        WriteAsset("decl/Fields.lua", R"LUA(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                count = 3,
                ratio = 0.5,
                label = { type = "string", default = "hi" },
                enabled = true,
                offset = { default = Vector3(0, 1, 0) },
                target = { type = "GameObject" },
                body = { type = "Rigidbody" },
                steps = { type = "int", default = 2 },
            }
            return Fields
        )LUA");
        WriteAsset("decl/Other.lua", R"LUA(
            local Other = {}
            Other.__index = Other
            Other.SerializableFields = {
                count = 9,
                brand = "new",
            }
            return Other
        )LUA");
        WriteAsset("decl/Odd.lua", R"LUA(
            local Odd = {}
            Odd.__index = Odd
            Odd.SerializableFields = { count = 3, [1] = "positional" }
            return Odd
        )LUA");
        WriteAsset("decl/Types.lua", R"LUA(
            local Types = {}
            Types.__index = Types
            Types.SerializableFields = {
                a = { type = "number" },
                b = { type = "float", default = 1.5 },
                c = { type = "integer" },
                d = { type = "boolean", default = false },
                e = { type = "Vector3" },
                f = { type = "SomethingUnknown", default = "kept" },
                g = { type = "SpriteComponent" },
                i = { default = nil },
                j = -2,
            }
            return Types
        )LUA");
        WriteAsset("decl/Computed.lua", R"LUA(
            local BASE = 2
            local function scaled(n) return n * BASE end
            local Computed = {}
            Computed.__index = Computed
            Computed.SerializableFields = {
                speed = scaled(4),
                name = "id" .. 7,
                up = { default = Vector3.new(0, 1, 0) },
            }
            return Computed
        )LUA");
        WriteAsset("decl/TopLevelEngine.lua", R"LUA(
            local limit = 89.0 * Math.DEG_TO_RAD
            local C = {}
            C.__index = C
            C.SerializableFields = { speed = 5, label = { type = "string", default = "x" } }
            return C
        )LUA");

        // A script that does everything a hostile one could at its top level, then declares one field
        WriteAsset("decl/Hostile.lua", R"LUA(
            L2_GLOBAL = "set by a script"
            Application.Quit()
            GameObject.Create("L2 stray object")
            local C = {}
            C.__index = C
            C.SerializableFields = { speed = 4 }
            function C:OnDestroy() L2_DESTROYED = true end
            function C:OnDisable() L2_DISABLED = true end
            return C
        )LUA");
        WriteAsset("decl/Loops.lua", "while true do end\nreturn {}\n");
        WriteAsset("decl/Broken.lua", "this is not lua\n");
        WriteAsset("decl/FromEngine.lua", R"LUA(
            local C = {}
            C.__index = C
            C.SerializableFields = Config.Fields
            return C
        )LUA");
        // One per test that edits it
        for (const char *name : {"Hot1", "Hot2", "Hot3"})
        {
            WriteAsset(std::string("decl/") + name + ".lua", R"LUA(
                local Hot = {}
                Hot.__index = Hot
                Hot.SerializableFields = { a = 1 }
                return Hot
            )LUA");
        }

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    std::unique_ptr<Tests::HangWatchdog> _watchdog;
    std::shared_ptr<Gate> _gate = std::make_shared<Gate>();
    std::vector<std::pair<std::string, Logger::LogLevel>> _logs;
    std::size_t _logSubscription = 0;
    bool _quitAllowed = true;

    static ScriptDeclarationCache &Cache() { return ScriptDeclarationCache::Instance(); }
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }
    static bool GlobalIsSet(const char *name) { return Lua()[name].get_type() != sol::type::lua_nil; }
    static IO::ResourcePath PathOf(const char *name) { return IO::ResourcePath(std::string("res://decl/") + name); }

    void SetUp() override
    {
        _watchdog = std::make_unique<Tests::HangWatchdog>(std::chrono::seconds(90));
        ASSERT_TRUE(LuaRuntime::RunsScripts()) << "another test left the policy changed";
        Cache().ResetForTest();
        _quitAllowed = Application::IsScriptQuitAllowed();
        Application::SetScriptQuitAllowed(true);
        Application::GetInstance().ResetQuitRequest();
        _logSubscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        {
            _logs.emplace_back(std::string(message), level);
        };
        Lua()["L2_GLOBAL"] = sol::lua_nil;
        Lua()["L2_DESTROYED"] = sol::lua_nil;
        Lua()["L2_DISABLED"] = sol::lua_nil;
    }

    void TearDown() override
    {
        LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::Run);
        // A worker still blocked in a hook is let go, and waited for, before the next test (and before the gate goes)
        _gate->Open();
        WaitForNoWorkers();
        Cache().ResetForTest();
        Logger::logEvent -= _logSubscription;
        Lua()["L2_GLOBAL"] = sol::lua_nil;
        Lua()["L2_DESTROYED"] = sol::lua_nil;
        Lua()["L2_DISABLED"] = sol::lua_nil;
        Application::SetScriptQuitAllowed(_quitAllowed);
        Application::GetInstance().ResetQuitRequest();
        _watchdog.reset();
    }

    static bool WaitForNoWorkers(const std::chrono::milliseconds limit = 20s)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (Cache().LiveWorkers() > 0 && std::chrono::steady_clock::now() < end)
        {
            std::this_thread::sleep_for(5ms);
        }
        return Cache().LiveWorkers() == 0;
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
            out += "\n  [" + std::to_string(static_cast<int>(logLevel)) + "] " + message;
        }
        return out;
    }

    // What a component that is given these scripts in turn (and this data first) holds
    static Snapshot Capture(const std::vector<const char *> &scripts, const json &initialData = json())
    {
        const auto go = GameObject::Create("Oracle");
        auto *component = go->AddComponent<LuaComponent>();
        if (!initialData.is_null())
        {
            component->SetScriptData(initialData);
        }
        for (const char *script : scripts)
        {
            component->SetScript(PathOf(script));
        }
        Snapshot snapshot;
        for (const FieldInfo &field : component->DescribeFields())
        {
            snapshot.fields.push_back(field.ToJson());
        }
        snapshot.data = component->GetScriptData();
        snapshot.missing = component->HasMissingScript();
        return snapshot;
    }

    static std::vector<std::string> FieldNames(const std::vector<FieldInfo> &fields)
    {
        std::vector<std::string> names;
        for (const FieldInfo &field : fields)
        {
            names.push_back(field.name);
        }
        return names;
    }

    // Blocks every evaluation on the gate (and returns a declared, empty result once it opens)
    void BlockEvaluations()
    {
        const std::shared_ptr<Gate> gate = _gate;
        Cache().SetEvaluatorForTest([gate](std::string_view, const std::string &, const DeclarationLimits &)
        {
            ++gate->entered;
            gate->Wait();
            DeclarationResult result;
            result.declared = true;
            return result;
        });
    }
};

// ==================== the policy ====================

TEST_F(ScriptDeclarationTest, TheDefaultPolicyRunsScripts)
{
    EXPECT_EQ(LuaRuntime::GetExecutionPolicy(), LuaExecutionPolicy::Run);
    EXPECT_TRUE(LuaRuntime::RunsScripts());

    const sol::protected_function_result ran = LuaRuntime::Instance().RunSource("L2_GLOBAL = 'ran'", "policy-test");
    EXPECT_TRUE(ran.valid());
    EXPECT_EQ(Lua()["L2_GLOBAL"].get<std::string>(), "ran");
}

TEST_F(ScriptDeclarationTest, NoWayOfRunningAScriptIsOpenUnderDeclarationsOnly)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    EXPECT_FALSE(LuaRuntime::RunsScripts());

    {
        const sol::protected_function_result ran = LuaRuntime::Instance().RunSource("L2_GLOBAL = 'ran'", "policy-test");
        EXPECT_FALSE(ran.valid());
    }
    {
        const sol::protected_function_result chunk = LuaRuntime::Instance().RunChunk("L2_GLOBAL = 'ran'", "policy-test");
        EXPECT_FALSE(chunk.valid());
    }
    EXPECT_FALSE(LuaRuntime::Instance().RunFile(PathOf("Hostile.lua")));
    EXPECT_FALSE(GlobalIsSet("L2_GLOBAL")) << "the script's top level never ran";
    EXPECT_FALSE(Application::GetInstance().IsQuitRequested());
}

TEST_F(ScriptDeclarationTest, ApplicationQuitIsARefusedAndLoggedWhenThePolicyIsNotRun)
{
    ASSERT_TRUE(Application::IsScriptQuitAllowed()) << "the older guard would let it through";
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);

    const sol::function quit = Lua()["Application"]["Quit"];
    ASSERT_TRUE(quit.valid());
    quit();

    EXPECT_FALSE(Application::GetInstance().IsQuitRequested());
    EXPECT_EQ(CountLogs("Application.Quit is refused", Logger::LogLevel::Warn), 1) << DumpLogs();

    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::Run);
    quit();
    EXPECT_TRUE(Application::GetInstance().IsQuitRequested()) << "a game still quits";
}

// ==================== the oracle: both policies give the same component ====================

TEST_F(ScriptDeclarationTest, EveryFormGivesTheSameFieldsAndDataUnderBothPolicies)
{
    struct Case
    {
        std::vector<const char *> scripts;
        json initialData;
    };
    const std::vector<Case> cases = {
        {{"Fields.lua"}, json()},
        {{"Other.lua"}, json()},
        {{"Odd.lua"}, json()},
        {{"Types.lua"}, json()},
        {{"Computed.lua"}, json()},
        {{"TopLevelEngine.lua"}, json()},
        // a different script keeps only the fields it declares (count is in both)
        {{"Fields.lua", "Other.lua"}, json()},
        // saved values win over defaults; the ones a script does not declare stay on its first script only
        {{"Fields.lua"}, json{{"count", 7}, {"stray", "kept"}}},
        {{"Fields.lua", "Other.lua"}, json{{"count", 7}, {"stray", "dropped"}}},
    };

    std::vector<Snapshot> run;
    for (const Case &testCase : cases)
    {
        run.push_back(Capture(testCase.scripts, testCase.initialData));
    }

    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const Snapshot declared = Capture(cases[i].scripts, cases[i].initialData);
        EXPECT_FALSE(declared.missing) << "case " << i;
        EXPECT_EQ(declared.fields, run[i].fields) << "case " << i;
        EXPECT_EQ(declared.data, run[i].data) << "case " << i;
    }
    EXPECT_FALSE(Application::GetInstance().IsQuitRequested());
}

// ==================== a component that reads declarations runs nothing ====================

TEST_F(ScriptDeclarationTest, AHostileScriptRunsNothingAndStillDeclaresItsField)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    std::size_t memoryBefore = 0;
    std::size_t memoryAfter = 0;

    {
        const auto go = GameObject::Create("Hostile");
        auto *component = go->AddComponent<LuaComponent>();
        memoryBefore = LuaRuntime::Instance().GetMemoryUsed();
        component->SetScript(PathOf("Hostile.lua"));

        EXPECT_FALSE(component->HasMissingScript()) << DumpLogs();
        EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "speed"}));
        EXPECT_EQ(component->GetScriptData(), (json{{"speed", 4}}));

        // The lifecycle a deleted object would run: no script code, so no global
        component->OnDisable();
        component->OnDestroy();
        memoryAfter = LuaRuntime::Instance().GetMemoryUsed();
    }

    EXPECT_FALSE(GlobalIsSet("L2_GLOBAL")) << "the top level never ran in the engine's state";
    EXPECT_FALSE(GlobalIsSet("L2_DESTROYED"));
    EXPECT_FALSE(GlobalIsSet("L2_DISABLED"));
    EXPECT_FALSE(Application::GetInstance().IsQuitRequested());
    EXPECT_EQ(memoryAfter, memoryBefore) << "the engine's Lua state allocated nothing";
}

TEST_F(ScriptDeclarationTest, ALoopingScriptIsAFailedDeclarationAndTheCallerGetsItBack)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto go = GameObject::Create("Looper");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"speed", 5}});

    component->SetScript(PathOf("Loops.lua"));

    EXPECT_TRUE(component->HasMissingScript());
    EXPECT_EQ(component->GetScriptData(), (json{{"speed", 5}})) << "a failed declaration keeps scriptData";
    EXPECT_GE(CountLogs("Loops.lua", Logger::LogLevel::Warn), 1) << "the reason is reported: " << DumpLogs();
}

TEST_F(ScriptDeclarationTest, AFailedDeclarationKeepsScriptDataAndShowsItsKeysWithInferredKinds)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const json saved = {{"speed", 5},
                        {"label", "x"},
                        {"flag", true},
                        {"ratio", 0.5},
                        {"origin", {{"x", 1}, {"y", 2}, {"z", 3}}},
                        {"target", {{"$ref", "123e4567-e89b-12d3-a456-426614174000"}}}};
    const auto go = GameObject::Create("Broken");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData(saved);

    component->SetScript(PathOf("Broken.lua"));

    EXPECT_TRUE(component->HasMissingScript());
    EXPECT_EQ(component->GetScriptData(), saved);
    const std::vector<FieldInfo> fields = component->DescribeFields();
    ASSERT_EQ(fields.size(), 7u);
    EXPECT_EQ(fields[0].name, "scriptUUID");
    // the keys of the data, sorted, each of the kind its value has; a reference is whatever the data holds
    EXPECT_EQ(FieldNames(fields), (std::vector<std::string>{"scriptUUID", "flag", "label", "origin", "ratio", "speed", "target"}));
    EXPECT_EQ(fields[1].kind, FieldKind::Bool);
    EXPECT_EQ(fields[2].kind, FieldKind::String);
    EXPECT_EQ(fields[3].kind, FieldKind::Vector3);
    EXPECT_EQ(fields[4].kind, FieldKind::Float);
    EXPECT_EQ(fields[5].kind, FieldKind::Int);
    EXPECT_EQ(fields[6].kind, FieldKind::Json);
    for (std::size_t i = 1; i < fields.size(); ++i)
    {
        EXPECT_EQ(fields[i].container, "scriptData");
    }
}

TEST_F(ScriptDeclarationTest, AListOnlyTheEngineCouldGiveIsReportedNotSilentlyEmpty)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto go = GameObject::Create("FromEngine");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"speed", 5}});

    component->SetScript(PathOf("FromEngine.lua"));

    EXPECT_EQ(component->GetScriptData(), (json{{"speed", 5}}));
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "speed"}));
    EXPECT_GE(CountLogs("unknown here", Logger::LogLevel::Warn), 1) << DumpLogs();
}

TEST_F(ScriptDeclarationTest, AScriptThatIsNotThereIsAMissingScriptWithItsDataKept)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto go = GameObject::Create("Gone");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"speed", 5}});

    component->SetScript(PathOf("DoesNotExist.lua"));

    EXPECT_TRUE(component->HasMissingScript());
    EXPECT_EQ(component->GetScriptData(), (json{{"speed", 5}}));
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID"}));
}

// ==================== hot reload through the cache ====================

TEST_F(ScriptDeclarationTest, ReloadingAScriptRereadsItsDeclarationsAndRunsNothing)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto go = GameObject::Create("Hot1");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(PathOf("Hot1.lua"));
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "a"}));
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    const std::size_t memoryBefore = LuaRuntime::Instance().GetMemoryUsed();

    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Hot1.lua"));
    ASSERT_NE(script, nullptr);
    const std::string original = script->GetSourceCode();
    script->SetSourceCode(R"LUA(
        L2_GLOBAL = "a reload ran this"
        local Hot = {}
        Hot.__index = Hot
        Hot.SerializableFields = { a = 5, b = { type = "string", default = "new" } }
        return Hot
    )LUA");
    LuaRuntime::Instance().ReloadModule(PathOf("Hot1.lua"), script.get());

    // The changed declaration: the new field is listed with its default, the saved value of the old one is kept
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "a", "b"}));
    EXPECT_EQ(component->GetScriptData(), (json{{"a", 1}, {"b", "new"}}));
    EXPECT_EQ(Cache().GetStats().evaluations, 2u) << "the new text was evaluated once";
    EXPECT_NE(Cache().Peek(PathOf("Hot1.lua"), script->GetSourceCode()), nullptr);
    EXPECT_EQ(Cache().Peek(PathOf("Hot1.lua"), original), nullptr) << "the old text's entry was dropped";

    EXPECT_FALSE(GlobalIsSet("L2_GLOBAL")) << "nothing ran, not even as a module";
    EXPECT_EQ(LuaRuntime::Instance().GetMemoryUsed(), memoryBefore);
    EXPECT_FALSE(LuaRuntime::Instance().GetModule(LuaRuntime::PathToModuleName(PathOf("Hot1.lua"))).has_value());
}

TEST_F(ScriptDeclarationTest, AReloadThatBreaksTheScriptKeepsTheDeclarationsOfTheLastGoodVersion)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto go = GameObject::Create("Hot2");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(PathOf("Hot2.lua"));
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Hot2.lua"));
    ASSERT_NE(script, nullptr);
    const std::string original = script->GetSourceCode();

    script->SetSourceCode("this is not lua");
    LuaRuntime::Instance().ReloadModule(PathOf("Hot2.lua"), script.get());

    EXPECT_FALSE(component->HasMissingScript());
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "a"}));
    EXPECT_EQ(CountLogs("Keeping the declarations", Logger::LogLevel::Warn), 1) << DumpLogs();

    // and a fixed version is read again
    script->SetSourceCode(original);
    LuaRuntime::Instance().ReloadModule(PathOf("Hot2.lua"), script.get());
    EXPECT_FALSE(component->HasMissingScript());
}

// ==================== the cache ====================

TEST_F(ScriptDeclarationTest, TheCacheHitsOnTheSameSourceAndMissesAfterAnEdit)
{
    const IO::ResourcePath path = PathOf("Cached.lua");
    const std::string first = "local C = {} C.SerializableFields = { a = 1 } return C";
    const std::string second = "local C = {} C.SerializableFields = { a = 2 } return C";

    const auto one = Cache().Get(path, first);
    const auto again = Cache().Get(path, first);
    EXPECT_EQ(one, again) << "the same entry";
    EXPECT_EQ(Cache().GetStats().lookups, 2u);
    EXPECT_EQ(Cache().GetStats().hits, 1u);
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    ASSERT_TRUE(one->declared);
    EXPECT_EQ(one->fields[0].defaultValue, 1);

    const auto edited = Cache().Get(path, second);
    EXPECT_NE(edited, one);
    EXPECT_EQ(Cache().GetStats().evaluations, 2u);
    EXPECT_EQ(edited->fields[0].defaultValue, 2);
    EXPECT_EQ(Cache().EntryCount(), 1u) << "one entry per path: the old text's is replaced";

    // keyed by the text, never a time: the old text is a miss again, and two texts one byte apart differ
    EXPECT_EQ(Cache().Peek(path, first), nullptr);
    EXPECT_NE(ScriptDeclarationCache::HashSource(first), ScriptDeclarationCache::HashSource(second));
    EXPECT_EQ(ScriptDeclarationCache::HashSource(first), ScriptDeclarationCache::HashSource(std::string(first)));
    EXPECT_NE(ScriptDeclarationCache::HashSource(""), ScriptDeclarationCache::HashSource(" "));
}

TEST_F(ScriptDeclarationTest, TwoComponentsOfOneScriptEvaluateItOnce)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    const auto first = GameObject::Create("One");
    const auto second = GameObject::Create("Two");
    first->AddComponent<LuaComponent>()->SetScript(PathOf("Fields.lua"));
    second->AddComponent<LuaComponent>()->SetScript(PathOf("Fields.lua"));

    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    EXPECT_EQ(Cache().GetStats().hits, 1u);
}

TEST_F(ScriptDeclarationTest, ADestroyedComponentIsNoLongerToldOfReloads)
{
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    std::atomic<int> told{0};
    Cache().Subscribe("decl.Subscribed", [&told] { ++told; }, &told);
    Cache().NotifyReloaded("decl.Subscribed");
    EXPECT_EQ(told.load(), 1);
    Cache().Unsubscribe(&told);
    Cache().NotifyReloaded("decl.Subscribed");
    EXPECT_EQ(told.load(), 1);

    {
        const auto go = GameObject::Create("Short lived");
        go->AddComponent<LuaComponent>()->SetScript(PathOf("Hot3.lua"));
    }
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(PathOf("Hot3.lua"));
    ASSERT_NE(script, nullptr);
    LuaRuntime::Instance().ReloadModule(PathOf("Hot3.lua"), script.get()); // would reach a dangling component if it stayed
}

// ==================== off the calling thread, the deadline, abandonment ====================

TEST_F(ScriptDeclarationTest, AnEvaluationRunsOffTheCallingThread)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build evaluates on the calling thread (the edit host is not a web target)";
#endif
    std::atomic<bool> sameThread{true};
    const std::thread::id caller = std::this_thread::get_id();
    Cache().SetEvaluatorForTest([&sameThread, caller](std::string_view, const std::string &, const DeclarationLimits &)
    {
        sameThread = std::this_thread::get_id() == caller;
        DeclarationResult result;
        result.declared = true;
        return result;
    });

    const auto result = Cache().Get(PathOf("Thread.lua"), "return {}");

    EXPECT_TRUE(result->declared);
    EXPECT_FALSE(sameThread.load());
    EXPECT_TRUE(WaitForNoWorkers(5s));
}

TEST_F(ScriptDeclarationTest, AnEvaluationThatOverrunsTheDeadlineIsAbandonedAndDoesNotStallTheCaller)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build has no worker to abandon";
#endif
    Cache().SetDeadlineMillis(150);
    BlockEvaluations();
    const IO::ResourcePath path = PathOf("Stall.lua");

    const auto start = std::chrono::steady_clock::now();
    const auto result = Cache().Get(path, "return {}");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 5s) << "the deadline is 150 ms (plus the grace); the worker is blocked for good";
    EXPECT_FALSE(result->declared);
    EXPECT_TRUE(result->timedOut);
    EXPECT_TRUE(result->budgetExceeded);
    ASSERT_FALSE(result->errors.empty());
    EXPECT_NE(result->errors[0].find("abandoned"), std::string::npos) << result->errors[0];
    EXPECT_EQ(Cache().LiveWorkers(), 1) << "the worker is still inside its call";
    EXPECT_EQ(Cache().GetStats().abandoned, 1u);

    // The timeout is recorded for that text: asking again is a hit, not a second worker
    const auto again = Cache().Get(path, "return {}");
    EXPECT_EQ(again, result);
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    EXPECT_EQ(Cache().LiveWorkers(), 1);

    // The worker finishes later with a declared result: it is dropped, the cache still says it timed out
    _gate->Open();
    ASSERT_TRUE(WaitForNoWorkers(10s)) << "the abandoned worker ended on its own once it could";
    const auto afterwards = Cache().Get(path, "return {}");
    EXPECT_FALSE(afterwards->declared);
    EXPECT_TRUE(afterwards->timedOut);
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);

    // An edit is a new text, so a new attempt (and the gate is open now, so it works)
    const auto edited = Cache().Get(path, "return { }");
    EXPECT_TRUE(edited->declared);
    EXPECT_EQ(Cache().GetStats().evaluations, 2u);
}

TEST_F(ScriptDeclarationTest, ACommandThatNeedsAStalledScriptAnswersWithTheFailedDeclarationShape)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build has no worker to abandon";
#endif
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
    Cache().SetDeadlineMillis(100);
    BlockEvaluations();
    const auto go = GameObject::Create("Stalled");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"speed", 5}});

    const auto start = std::chrono::steady_clock::now();
    component->SetScript(PathOf("Fields.lua"));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 5s);
    EXPECT_TRUE(component->HasMissingScript());
    EXPECT_EQ(component->GetScriptData(), (json{{"speed", 5}}));
    EXPECT_EQ(FieldNames(component->DescribeFields()), (std::vector<std::string>{"scriptUUID", "speed"}));
}

TEST_F(ScriptDeclarationTest, AtMostMaxWorkersEvaluationsExistAndTheRestFailFast)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build has no worker to abandon";
#endif
    Cache().SetDeadlineMillis(40);
    BlockEvaluations();
    for (int i = 0; i < ScriptDeclarationCache::MaxWorkers; ++i)
    {
        const auto result =
            Cache().Get(IO::ResourcePath("res://decl/Blocked" + std::to_string(i) + ".lua"), "return {}");
        EXPECT_TRUE(result->timedOut) << i;
    }
    ASSERT_EQ(Cache().LiveWorkers(), ScriptDeclarationCache::MaxWorkers);
    const std::size_t entries = Cache().EntryCount();

    const auto start = std::chrono::steady_clock::now();
    const auto refused = Cache().Get(PathOf("OneTooMany.lua"), "return {}");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 2s) << "no thread is waited on";
    EXPECT_FALSE(refused->declared);
    EXPECT_FALSE(refused->timedOut);
    ASSERT_FALSE(refused->errors.empty());
    EXPECT_NE(refused->errors[0].find("still running"), std::string::npos) << refused->errors[0];
    EXPECT_EQ(Cache().GetStats().refused, 1u);
    EXPECT_EQ(Cache().LiveWorkers(), ScriptDeclarationCache::MaxWorkers) << "no fifth worker was started";
    EXPECT_EQ(Cache().EntryCount(), entries) << "a refusal is not remembered: a later attempt may succeed";

    _gate->Open();
    ASSERT_TRUE(WaitForNoWorkers(10s));
    const auto later = Cache().Get(PathOf("OneTooMany.lua"), "return {}");
    EXPECT_TRUE(later->declared);
}

TEST_F(ScriptDeclarationTest, ShutdownWaitsABoundedTimeForAnAbandonedWorkerAndThenRefusesNewWork)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build has no worker to abandon";
#endif
    Cache().SetDeadlineMillis(40);
    BlockEvaluations();
    (void)Cache().Get(PathOf("Stuck.lua"), "return {}");
    ASSERT_EQ(Cache().LiveWorkers(), 1);

    const auto start = std::chrono::steady_clock::now();
    const int left = Cache().Shutdown(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(left, 1) << "a thread cannot be stopped: it is reported and left";
    EXPECT_GE(elapsed, 90ms);
    EXPECT_LT(elapsed, 5s) << "bounded";

    const auto refused = Cache().Get(PathOf("AfterShutdown.lua"), "return {}");
    EXPECT_FALSE(refused->declared);
    ASSERT_FALSE(refused->errors.empty());
    EXPECT_NE(refused->errors[0].find("shutting down"), std::string::npos) << refused->errors[0];
    EXPECT_EQ(Cache().LiveWorkers(), 1) << "no new worker";

    // Once the worker can end, a second Shutdown finds none left: the abandoned worker is cleaned up
    _gate->Open();
    EXPECT_EQ(Cache().Shutdown(10s), 0);
    EXPECT_EQ(Cache().LiveWorkers(), 0);
}

TEST_F(ScriptDeclarationTest, SeveralThreadsAskingTheCacheAtOnceAllGetAnAnswer)
{
#if defined(N2ENGINE_SINGLE_THREADED)
    GTEST_SKIP() << "The single-threaded web build cannot start threads";
#endif
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < ScriptDeclarationCache::MaxWorkers; ++i)
    {
        threads.emplace_back([&failures, i]
        {
            for (int round = 0; round < 4; ++round)
            {
                const auto result = Cache().Get(IO::ResourcePath("res://decl/Many" + std::to_string(i) + ".lua"),
                                                "local C = {} C.SerializableFields = { n = " + std::to_string(round) +
                                                    " } return C");
                if (result == nullptr || !result->declared || result->fields.size() != 1)
                {
                    ++failures;
                }
            }
        });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(WaitForNoWorkers(10s));
}
