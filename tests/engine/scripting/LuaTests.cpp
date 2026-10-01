#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// The Lua state is a process-wide singleton, so globals set by one test are visible to
// later ones. Tests use distinct global names to stay independent.
class LuaTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    // Runs a chunk and fails the test with the Lua error message if it errors
    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result =
            Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }
};

// ============================================================================
// Runtime and globals
// ============================================================================

TEST_F(LuaTest, RegistersEngineGlobals)
{
    for (const char *name : {
             "Math", "Random", "Vector2", "Vector3", "Vector4", "Quaternion", "Matrix4", "Color",
             "GameObject", "Scene", "SceneManager", "Time", "Debug", "Input", "Application",
             "Window", "Camera", "Audio", "BodyType", "ActionPhase", "WindowMode", "require",
         })
    {
        EXPECT_TRUE(Lua()[name].valid()) << name;
    }
}

TEST_F(LuaTest, InitializeTwiceKeepsUsertypesWorking)
{
    Run("v_before_reinit = Vector3(1, 2, 3)");

    // Re-registering usertypes in the same state used to strip their metatables
    EXPECT_TRUE(LuaRuntime::Instance().Initialize());

    EXPECT_FLOAT_EQ(Eval<float>("v_before_reinit.y"), 2.0f);
    EXPECT_FLOAT_EQ(Eval<float>("Vector3(4, 5, 6):Length()"), Math::Vector3(4, 5, 6).Length());
    EXPECT_EQ(Eval<std::string>("GameObject.Create('AfterReinit'):GetName()"), "AfterReinit");

    Lua()["v_before_reinit"] = sol::lua_nil;
}

TEST_F(LuaTest, OpensOnlySafeStandardLibraries)
{
    EXPECT_EQ(Eval<std::string>("type(math.floor)"), "function");
    EXPECT_EQ(Eval<std::string>("type(string.format)"), "function");
    EXPECT_EQ(Eval<std::string>("type(table.insert)"), "function");

    // io and os aren't opened, so scripts can't touch the filesystem or run processes
    EXPECT_EQ(Eval<std::string>("type(io)"), "nil");
    EXPECT_EQ(Eval<std::string>("type(os)"), "nil");
}

// ============================================================================
// Math
// ============================================================================

TEST_F(LuaTest, Vector3ConstructionAndFields)
{
    EXPECT_FLOAT_EQ(Eval<float>("Vector3(1, 2, 3).y"), 2.0f);
    EXPECT_FLOAT_EQ(Eval<float>("Vector3.new(4, 5, 6).z"), 6.0f);

    const Math::Vector3 v = Eval<Math::Vector3>("Vector3(7, 8, 9)");
    EXPECT_FLOAT_EQ(v.x, 7.0f);
    EXPECT_FLOAT_EQ(v.y, 8.0f);
    EXPECT_FLOAT_EQ(v.z, 9.0f);
}

TEST_F(LuaTest, Vector3Operators)
{
    EXPECT_FLOAT_EQ(Eval<float>("(Vector3(1, 2, 3) + Vector3(1, 1, 1)).z"), 4.0f);
    EXPECT_FLOAT_EQ(Eval<float>("(Vector3(5, 5, 5) - Vector3(1, 2, 3)).y"), 3.0f);
    EXPECT_FLOAT_EQ(Eval<float>("(Vector3(1, 2, 3) * 2).x"), 2.0f);
    EXPECT_FLOAT_EQ(Eval<float>("(2 * Vector3(1, 2, 3)).y"), 4.0f);
    EXPECT_FLOAT_EQ(Eval<float>("(-Vector3(1, 2, 3)).z"), -3.0f);
    EXPECT_TRUE(Eval<bool>("Vector3(1, 2, 3) == Vector3(1, 2, 3)"));
    EXPECT_FALSE(Eval<bool>("Vector3(1, 2, 3) == Vector3(3, 2, 1)"));
}

TEST_F(LuaTest, Vector3Methods)
{
    EXPECT_FLOAT_EQ(Eval<float>("Vector3(3, 4, 0):Length()"), 5.0f);
    EXPECT_FLOAT_EQ(Eval<float>("Vector3(1, 2, 3):Dot(Vector3(4, 5, 6))"), 32.0f);
    EXPECT_FLOAT_EQ(Eval<float>("Vector3(1, 0, 0):Cross(Vector3(0, 1, 0)).z"), 1.0f);
    EXPECT_NEAR(Eval<float>("Vector3(10, 0, 0):Normalized():Length()"), 1.0f, 1e-5f);
}

TEST_F(LuaTest, QuaternionRotationPreservesLength)
{
    Run("q_rotated = Quaternion.FromAxisAngle(Vector3(0, 1, 0), Math.HALF_PI):Rotate(Vector3(1, 0, 0))");

    EXPECT_NEAR(Eval<float>("q_rotated:Length()"), 1.0f, 1e-5f);
    EXPECT_NEAR(Eval<float>("q_rotated.x"), 0.0f, 1e-5f); // 90 degrees about Y moves X off the X axis
    EXPECT_NEAR(Eval<float>("q_rotated.y"), 0.0f, 1e-5f);
}

TEST_F(LuaTest, MathConstants)
{
    EXPECT_NEAR(Eval<float>("Math.PI"), 3.14159265f, 1e-5f);
    EXPECT_NEAR(Eval<float>("Math.DEG_TO_RAD * 180"), 3.14159265f, 1e-5f);
}

// ============================================================================
// Time
// ============================================================================

TEST_F(LuaTest, TimeValuesAreNumbers)
{
    for (const char *field : {"deltaTime", "time", "fixedDeltaTime", "unscaledDeltaTime",
                              "unscaledTime", "fixedUnscaledDeltaTime", "timeScale"})
    {
        EXPECT_EQ(Eval<std::string>(std::string("type(Time.") + field + ")"), "number") << field;
    }
}

TEST_F(LuaTest, TimeScaleIsWritable)
{
    const float original = Time::GetTimeScale();

    Run("Time.timeScale = 0.25");
    EXPECT_FLOAT_EQ(Time::GetTimeScale(), 0.25f);
    EXPECT_FLOAT_EQ(Eval<float>("Time.timeScale"), 0.25f);

    Time::SetTimeScale(original);
}

// ============================================================================
// Enums, logging and globals that need Application::Init
// ============================================================================

TEST_F(LuaTest, EnumsAreExposed)
{
    EXPECT_TRUE(Lua()["BodyType"]["Dynamic"].valid());
    EXPECT_TRUE(Lua()["BodyType"]["Static"].valid());
    EXPECT_TRUE(Lua()["ActionPhase"]["Started"].valid());
    EXPECT_TRUE(Lua()["WindowMode"]["Fullscreen"].valid());
}

TEST_F(LuaTest, DebugLoggingDoesNotError)
{
    Run("Debug.Log('log from test') Debug.Warn('warn from test') Debug.Error('error from test')");
}

TEST_F(LuaTest, MainCameraIsNilWithoutApplicationInit)
{
    // Tests never call Application::Init, so there's no main camera
    EXPECT_TRUE(Eval<bool>("Camera.Main() == nil"));
}

// ============================================================================
// GameObjects and scenes
// ============================================================================

TEST_F(LuaTest, GameObjectConstruction)
{
    EXPECT_EQ(Eval<std::string>("GameObject('Hero'):GetName()"), "Hero");
    EXPECT_EQ(Eval<std::string>("GameObject.Create('Villain'):GetName()"), "Villain");
    EXPECT_EQ(Eval<std::string>("GameObject.Create():GetName()"), "GameObject");
}

TEST_F(LuaTest, GameObjectIsSharedWithCpp)
{
    Run("go_shared = GameObject.Create('Shared')");

    const auto go = Lua()["go_shared"].get<GameObjectRef>().Lock();
    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetName(), "Shared");

    go->SetName("RenamedInCpp");
    EXPECT_EQ(Eval<std::string>("go_shared:GetName()"), "RenamedInCpp");

    Lua()["go_shared"] = sol::lua_nil;
}

TEST_F(LuaTest, GameObjectPositionFromLua)
{
    Run(R"(
        go_positioned = GameObject.Create('Positioned')
        go_positioned:CreatePositionable()
        go_positioned:GetPositionable():SetPosition(Vector3(1, 2, 3))
    )");

    const auto go = Lua()["go_positioned"].get<GameObjectRef>().Lock();
    ASSERT_TRUE(go->HasPositionable());
    const Math::Vector3 pos = go->GetPositionable()->GetPosition();
    EXPECT_FLOAT_EQ(pos.x, 1.0f);
    EXPECT_FLOAT_EQ(pos.y, 2.0f);
    EXPECT_FLOAT_EQ(pos.z, 3.0f);

    Lua()["go_positioned"] = sol::lua_nil;
}

TEST_F(LuaTest, GameObjectHierarchyFromLua)
{
    Run(R"(
        go_parent = GameObject.Create('Parent')
        local child = GameObject.Create('Child')
        go_parent:AddChild(child)
    )");

    EXPECT_EQ(Eval<std::string>("go_parent:FindChild('Child'):GetParent():GetName()"), "Parent");

    const auto parent = Lua()["go_parent"].get<GameObjectRef>().Lock();
    ASSERT_EQ(parent->GetChildren().size(), 1u);
    EXPECT_EQ(parent->GetChildren()[0]->GetName(), "Child");

    Lua()["go_parent"] = sol::lua_nil;
}

TEST_F(LuaTest, SceneFromLua)
{
    // Tests never load a scene until this one, so Lua sees nil rather than crashing
    if (SceneManager::GetCurScene() == nullptr)
    {
        EXPECT_TRUE(Eval<bool>("SceneManager.GetCurrentScene() == nil"));
    }

    SceneManager::AddScene(Scene::Create("LuaTestScene"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);

    Run("SceneManager.GetCurrentScene():AddRootGameObject(GameObject.Create('FromLua'))");

    const auto found = SceneManager::GetCurSceneRef().FindGameObject("FromLua");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(Eval<std::string>("SceneManager.GetCurrentScene():FindGameObject('FromLua'):GetName()"), "FromLua");
}

// ============================================================================
// Scripts on disk: LuaComponent and require
// ============================================================================

class LuaScriptTest : public LuaTest
{
protected:
    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    static void SetUpTestSuite()
    {
        LuaTest::SetUpTestSuite();

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // Scripts return a class table; LuaComponent makes it the instance's metatable
        WriteAsset("scripts/Behaviour.lua", R"(
            local Behaviour = {}
            Behaviour.__index = Behaviour

            function Behaviour:OnAttach()
                self.gameObject:SetName("attached")
            end

            function Behaviour:OnUpdate()
                self.updates = (self.updates or 0) + 1
                self.gameObject:SetName("updates=" .. self.updates)
            end

            return Behaviour
        )");

        WriteAsset("scripts/Fields.lua", R"(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                speed = { default = 2.5 },
            }

            function Fields:OnAttach()
                self.gameObject:SetName(string.format("speed=%.1f", self.speed))
            end

            return Fields
        )");

        WriteAsset("scripts/Broken.lua", R"(
            local Broken = {}
            Broken.__index = Broken

            function Broken:OnUpdate()
                error("boom")
            end

            return Broken
        )");

        WriteAsset("scripts/NotATable.lua", "return 42");
        WriteAsset("scripts/SyntaxError.lua", "local x = = 1");

        WriteAsset("util/helpers.lua", R"(
            local helpers = {}
            function helpers.double(x) return x * 2 end
            return helpers
        )");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static LuaComponent *AttachScript(const GameObject::Ptr &go, const std::string &path)
    {
        auto *component = go->AddComponent<LuaComponent>();
        component->SetScript(IO::ResourcePath(path));
        return component;
    }
};

TEST_F(LuaScriptTest, ComponentRunsLifecycleMethods)
{
    const auto go = GameObject::Create("Scripted");
    auto *component = AttachScript(go, "res://scripts/Behaviour.lua");
    ASSERT_FALSE(component->HasMissingScript());

    component->OnAttach();
    EXPECT_EQ(go->GetName(), "attached");

    component->OnUpdate();
    component->OnUpdate();
    EXPECT_EQ(go->GetName(), "updates=2");
}

TEST_F(LuaScriptTest, InstancesDoNotShareState)
{
    const auto a = GameObject::Create("A");
    const auto b = GameObject::Create("B");
    auto *scriptA = AttachScript(a, "res://scripts/Behaviour.lua");
    auto *scriptB = AttachScript(b, "res://scripts/Behaviour.lua");

    scriptA->OnUpdate();
    scriptA->OnUpdate();
    scriptB->OnUpdate();

    EXPECT_EQ(a->GetName(), "updates=2");
    EXPECT_EQ(b->GetName(), "updates=1");
}

TEST_F(LuaScriptTest, SerializableFieldDefaultsAndOverrides)
{
    const auto go = GameObject::Create("WithFields");
    auto *component = AttachScript(go, "res://scripts/Fields.lua");
    ASSERT_FALSE(component->HasMissingScript());

    EXPECT_FLOAT_EQ(component->GetField<float>("speed"), 2.5f);
    component->OnAttach();
    EXPECT_EQ(go->GetName(), "speed=2.5");

    component->SetField<float>("speed", 7.5f);
    EXPECT_FLOAT_EQ(component->GetField<float>("speed"), 7.5f);
    component->OnAttach();
    EXPECT_EQ(go->GetName(), "speed=7.5");
}

TEST_F(LuaScriptTest, MissingScriptIsReportedAndSafe)
{
    const auto go = GameObject::Create("Missing");
    auto *component = AttachScript(go, "res://scripts/DoesNotExist.lua");

    EXPECT_TRUE(component->HasMissingScript());
    component->OnAttach();
    component->OnUpdate();
    EXPECT_EQ(go->GetName(), "Missing");
}

TEST_F(LuaScriptTest, ScriptMustReturnATable)
{
    const auto go = GameObject::Create("NotATable");

    EXPECT_TRUE(AttachScript(go, "res://scripts/NotATable.lua")->HasMissingScript());
}

TEST_F(LuaScriptTest, SyntaxErrorIsReportedAsMissing)
{
    const auto go = GameObject::Create("SyntaxError");

    EXPECT_TRUE(AttachScript(go, "res://scripts/SyntaxError.lua")->HasMissingScript());
}

TEST_F(LuaScriptTest, RuntimeErrorIsContained)
{
    const auto go = GameObject::Create("Broken");
    auto *component = AttachScript(go, "res://scripts/Broken.lua");
    ASSERT_FALSE(component->HasMissingScript());

    EXPECT_NO_THROW(component->OnUpdate()); // logged, not thrown
    EXPECT_EQ(go->GetName(), "Broken");
}

TEST_F(LuaScriptTest, RequireLoadsModulesFromAssets)
{
    EXPECT_EQ(Eval<int>("require('util.helpers').double(21)"), 42);

    // Second require returns the cached module
    EXPECT_TRUE(Eval<bool>("require('util.helpers') == require('util.helpers')"));
}

TEST_F(LuaScriptTest, RequireOfMissingModuleReturnsNil)
{
    EXPECT_TRUE(Eval<bool>("require('util.does_not_exist') == nil"));
}
