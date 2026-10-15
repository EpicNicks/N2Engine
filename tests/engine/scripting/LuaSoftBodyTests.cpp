#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/physics/SoftBody.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"

// SoftBody from Lua: go:AddComponent("SoftBody"), its settings, and AddVelocity/Reset/GetActiveBackendName on a body
// that is attached to a playing scene (Scene::ProcessAttachQueue starts it, Scene::FixedUpdate steps it)

using namespace N2Engine;
using namespace N2Engine::Scripting;
using Physics::SoftBody;

class LuaSoftBodyTest : public ::testing::Test
{
protected:
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    void SetUp() override
    {
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
        Time::SetTimeScale(1.0f);
        ASSERT_TRUE(Time::SetFixedTimestep(0.02));
    }

    void TearDown() override
    {
        Run("lua_obj = nil; lua_body = nil; lua_mr = nil");
        Lua().collect_garbage();
        // The scene's components go while the application is still up
        SceneManager::AddScene(Scene::Create("LuaSoftBodyTest_Empty_" + std::to_string(++_scenes)), true);
        SceneManager::ProcessAnyPendingSceneChange();
    }

    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected an error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
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

    /// A scene with the script's objects in it, attached, so the component has started
    static Scene &PlayScene(const std::string &script)
    {
        SceneManager::AddScene(Scene::Create("LuaSoftBodyTest_Scene_" + std::to_string(++_scenes)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Scene &scene = SceneManager::GetCurSceneRef();
        Run(script);
        scene.ProcessAttachQueue();
        return scene;
    }

    /// The object of lua_obj, a MeshRenderer on the built-in cube and a SoftBody, in a playing scene
    static Scene &PlayJelly(const std::string &extra = "")
    {
        return PlayScene(R"(
            lua_obj = GameObject.Create("LuaJelly")
            lua_mr = lua_obj:AddComponent("MeshRenderer")
            lua_mr:SetMesh("Cube")
            lua_body = lua_obj:AddComponent("SoftBody")
            lua_body:SetResolution(4)
            lua_body:SetStiffness(1000000)
        )" + extra + R"(
            SceneManager.GetCurrentScene():AddRootGameObject(lua_obj)
        )");
    }

    static float RuntimeMeanY()
    {
        const auto obj = Lua()["lua_obj"].get<GameObjectRef>().Lock();
        const auto *body = obj ? obj->GetComponent<SoftBody>() : nullptr;
        if (!body || !body->GetRuntimeMesh())
        {
            return 0.0f;
        }
        double y = 0.0;
        for (const auto &vertex : body->GetRuntimeMesh()->GetVertices())
        {
            y += vertex.position[1];
        }
        return static_cast<float>(y / static_cast<double>(body->GetRuntimeMesh()->GetVertices().size()));
    }

    static inline int _scenes = 0;
};

TEST_F(LuaSoftBodyTest, SetsAndGetsItsSettings)
{
    Run(R"(
        lua_obj = GameObject.Create("LuaJelly")
        lua_body = lua_obj:AddComponent("SoftBody")
        lua_body:SetBackend("CPU")
        lua_body:SetResolution(7)
        lua_body:SetDensity(250.5)
        lua_body:SetStiffness(30000)
        lua_body:SetPoisson(0.45)
        lua_body:SetDamping(2.5)
        lua_body:SetFriction(0.25)
        lua_body:SetSolverIterations(5)
        lua_body:SetGround(true, -1.5)
        lua_body:SetMaterialModel("NeoHookean")
        lua_body:SetSelfCollision(true)
    )");

    const auto obj = Lua()["lua_obj"].get<GameObjectRef>().Lock();
    ASSERT_NE(obj, nullptr);
    const auto *body = obj->GetComponent<SoftBody>();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->backend, SoftBodies::SoftBodyBackend::CPU);
    EXPECT_EQ(body->resolution, 7);
    EXPECT_FLOAT_EQ(body->density, 250.5f);
    EXPECT_FLOAT_EQ(body->stiffness, 30000.0f);
    EXPECT_FLOAT_EQ(body->poisson, 0.45f);
    EXPECT_FLOAT_EQ(body->damping, 2.5f);
    EXPECT_FLOAT_EQ(body->friction, 0.25f);
    EXPECT_EQ(body->solverIterations, 5);
    EXPECT_TRUE(body->collideWithGround);
    EXPECT_FLOAT_EQ(body->groundHeight, -1.5f);
    EXPECT_EQ(body->materialModel, Physics::SoftMaterialModel::NeoHookean);
    EXPECT_TRUE(body->selfCollision);

    EXPECT_EQ(Eval<std::string>("lua_body:GetBackend()"), "CPU");
    EXPECT_EQ(Eval<int>("lua_body:GetResolution()"), 7);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetDensity()"), 250.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetStiffness()"), 30000.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetPoisson()"), 0.45f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetDamping()"), 2.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetFriction()"), 0.25f);
    EXPECT_EQ(Eval<int>("lua_body:GetSolverIterations()"), 5);
    EXPECT_TRUE(Eval<bool>("(lua_body:GetGround())"));
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_body:GetGround())"), -1.5f);
    EXPECT_EQ(Eval<std::string>("lua_body:GetMaterialModel()"), "NeoHookean");
    EXPECT_TRUE(Eval<bool>("lua_body:GetSelfCollision()"));
    EXPECT_TRUE(Eval<bool>("lua_obj:GetComponent('SoftBody') == lua_body"));
}

TEST_F(LuaSoftBodyTest, TheDefaultsAreWhatTheComponentHas)
{
    Run(R"(
        lua_obj = GameObject.Create("LuaJelly")
        lua_body = lua_obj:AddComponent("SoftBody")
    )");
    EXPECT_EQ(Eval<std::string>("lua_body:GetBackend()"), "Auto");
    EXPECT_EQ(Eval<int>("lua_body:GetResolution()"), 10);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetDensity()"), 1000.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetStiffness()"), 100000.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_body:GetPoisson()"), 0.3f);
    EXPECT_FALSE(Eval<bool>("(lua_body:GetGround())"));
    EXPECT_EQ(Eval<std::string>("lua_body:GetMaterialModel()"), "CoRotational");
    EXPECT_FALSE(Eval<bool>("lua_body:GetSelfCollision()"));
    EXPECT_EQ(Eval<std::string>("lua_body:GetActiveBackendName()"), "") << "not started: no backend yet";
}

TEST_F(LuaSoftBodyTest, AnUnknownEnumNameIsAnErrorThatListsTheOptionsAndChangesNothing)
{
    Run(R"(
        lua_obj = GameObject.Create("LuaJelly")
        lua_body = lua_obj:AddComponent("SoftBody")
    )");
    std::string error = RunExpectingError(R"(lua_body:SetBackend("Gpu"))");
    EXPECT_NE(error.find("SoftBody:SetBackend"), std::string::npos) << error;
    EXPECT_NE(error.find("'Gpu'"), std::string::npos) << error;
    EXPECT_NE(error.find("Auto, PhysX, CPU"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_body:GetBackend()"), "Auto");

    error = RunExpectingError(R"(lua_body:SetMaterialModel("Stiff"))");
    EXPECT_NE(error.find("SoftBody:SetMaterialModel"), std::string::npos) << error;
    EXPECT_NE(error.find("CoRotational, NeoHookean"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_body:GetMaterialModel()"), "CoRotational");
}

TEST_F(LuaSoftBodyTest, ABodyOnAPlayingSceneRunsOnTheCpuAndTheMeshRendererKeepsItsMesh)
{
    Scene &scene = PlayJelly();
    EXPECT_EQ(Eval<std::string>("lua_body:GetActiveBackendName()"), "CPU") << "started when the scene attached it";
    EXPECT_EQ(Eval<std::string>("lua_mr:GetMesh()"), "Cube") << "the saved mesh is untouched by the override";

    const float before = RuntimeMeanY();
    for (int i = 0; i < 15; ++i)
    {
        scene.FixedUpdate();
    }
    EXPECT_LT(RuntimeMeanY(), before - 0.05f) << "the body fell";
    EXPECT_EQ(Eval<std::string>("lua_mr:GetMesh()"), "Cube");
}

TEST_F(LuaSoftBodyTest, APhysXRequestFallsBackToCpuAndTheNameSaysWhatRuns)
{
    PlayJelly(R"(lua_body:SetBackend("PhysX"))");
    // The script asked before the scene attached the body, so the PhysX request is what the body started with
    EXPECT_EQ(Eval<std::string>("lua_body:GetBackend()"), "PhysX") << "what was asked for";
    EXPECT_EQ(Eval<std::string>("lua_body:GetActiveBackendName()"), "CPU") << "no GPU backend here: what is used";
}

TEST_F(LuaSoftBodyTest, AddVelocityPushesTheBodyAndResetPutsItBack)
{
    Scene &scene = PlayJelly();
    const auto obj = Lua()["lua_obj"].get<GameObjectRef>().Lock();
    ASSERT_NE(obj, nullptr);
    const auto *body = obj->GetComponent<SoftBody>();
    ASSERT_NE(body, nullptr);
    const auto meanX = [&]()
    {
        double x = 0.0;
        for (const auto &vertex : body->GetRuntimeMesh()->GetVertices())
        {
            x += vertex.position[0];
        }
        return static_cast<float>(x / static_cast<double>(body->GetRuntimeMesh()->GetVertices().size()));
    };

    Run("lua_body:AddVelocity(Vector3(3, 0, 0))");
    for (int i = 0; i < 10; ++i)
    {
        scene.FixedUpdate();
    }
    EXPECT_GT(meanX(), 0.4f) << "pushed along +x";
    EXPECT_LT(RuntimeMeanY(), -0.05f) << "and fell";

    Run("lua_body:Reset()");
    EXPECT_EQ(Eval<std::string>("lua_body:GetActiveBackendName()"), "CPU");
    scene.FixedUpdate();
    EXPECT_NEAR(meanX(), 0.0f, 0.05f) << "back at the rest shape";
    EXPECT_NEAR(RuntimeMeanY(), 0.0f, 0.02f);
}

TEST_F(LuaSoftBodyTest, WithoutAMeshRendererTheBodyIsInertAndTheNameIsEmpty)
{
    PlayScene(R"(
        lua_obj = GameObject.Create("LuaBare")
        lua_body = lua_obj:AddComponent("SoftBody")
        SceneManager.GetCurrentScene():AddRootGameObject(lua_obj)
    )");
    EXPECT_EQ(Eval<std::string>("lua_body:GetActiveBackendName()"), "");
    EXPECT_NO_THROW(Run("lua_body:AddVelocity(Vector3(1, 0, 0)); lua_body:Reset()"));
}

TEST_F(LuaSoftBodyTest, TheBodyGoesWithItsSceneAndTheCountGoesBack)
{
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    PlayJelly();
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    Run("lua_obj = nil; lua_body = nil; lua_mr = nil");
    Lua().collect_garbage();
    SceneManager::AddScene(Scene::Create("LuaSoftBodyTest_Next_" + std::to_string(++_scenes)), true);
    SceneManager::ProcessAnyPendingSceneChange();
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
}
