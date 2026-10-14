#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// ParticleSystem from Lua: go:AddComponent("ParticleSystem"), its settings, and Play/Stop/Clear/EmitBurst on a system
// that is attached to a playing scene (Scene::ProcessAttachQueue starts it, Scene::FixedUpdate steps it)

using namespace N2Engine;
using namespace N2Engine::Scripting;
using Rendering::ParticleSystem;

class LuaParticleSystemTest : public ::testing::Test
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
        Run("lua_fx = nil; lua_ps = nil; lua_text = nil");
        Lua().collect_garbage();
        // The scene's components go while the application is still up
        SceneManager::AddScene(Scene::Create("LuaParticleSystemTest_Empty_" + std::to_string(++_scenes)), true);
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

    /// A scene with the script's object in it, attached, so the component has started
    static Scene &PlayScene(const std::string &script)
    {
        SceneManager::AddScene(Scene::Create("LuaParticleSystemTest_Scene_" + std::to_string(++_scenes)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Scene &scene = SceneManager::GetCurSceneRef();
        Run(script);
        scene.ProcessAttachQueue();
        return scene;
    }

    static inline int _scenes = 0;
};

TEST_F(LuaParticleSystemTest, SetsAndGetsItsSettings)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetBackend("CPU")
        lua_ps:SetPlayOnAwake(false)
        lua_ps:SetMaxParticles(250)
        lua_ps:SetSeed(17)
        lua_ps:SetEmissionRate(12.5)
        lua_ps:SetBurstCount(3)
        lua_ps:SetShape("Sphere")
        lua_ps:SetShapeRadius(0.75)
        lua_ps:SetConeAngle(40)
        lua_ps:SetLifetime(0.5, 2.5)
        lua_ps:SetSpeed(1, 3)
        lua_ps:SetSize(0.3, 0.6)
        lua_ps:SetStartColor(Color.new(1.0, 0.5, 0.25, 0.75))
        lua_ps:SetEndColor(Color.new(0.0, 0.0, 1.0, 0.0))
        lua_ps:SetGravityScale(0.25)
        lua_ps:SetDrag(0.5)
        lua_ps:SetGround(true, -1.5, 0.6)
        lua_ps:SetBlendMode("Additive")
    )");

    const auto fx = Lua()["lua_fx"].get<GameObjectRef>().Lock();
    ASSERT_NE(fx, nullptr);
    const auto *ps = fx->GetComponent<ParticleSystem>();
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->backend, Particles::ParticleBackend::CPU);
    EXPECT_FALSE(ps->playOnAwake);
    EXPECT_EQ(ps->maxParticles, 250);
    EXPECT_EQ(ps->seed, 17);
    EXPECT_FLOAT_EQ(ps->emissionRate, 12.5f);
    EXPECT_EQ(ps->burstCount, 3);
    EXPECT_EQ(ps->shape, Particles::EmitterShape::Sphere);
    EXPECT_FLOAT_EQ(ps->shapeRadius, 0.75f);
    EXPECT_FLOAT_EQ(ps->coneAngle, 40.0f);
    EXPECT_FLOAT_EQ(ps->lifetimeMin, 0.5f);
    EXPECT_FLOAT_EQ(ps->lifetimeMax, 2.5f);
    EXPECT_FLOAT_EQ(ps->speedMin, 1.0f);
    EXPECT_FLOAT_EQ(ps->speedMax, 3.0f);
    EXPECT_FLOAT_EQ(ps->startSize, 0.3f);
    EXPECT_FLOAT_EQ(ps->endSize, 0.6f);
    EXPECT_FLOAT_EQ(ps->startColor.g, 0.5f);
    EXPECT_FLOAT_EQ(ps->endColor.b, 1.0f);
    EXPECT_FLOAT_EQ(ps->gravityScale, 0.25f);
    EXPECT_FLOAT_EQ(ps->drag, 0.5f);
    EXPECT_TRUE(ps->collideWithGround);
    EXPECT_FLOAT_EQ(ps->groundHeight, -1.5f);
    EXPECT_FLOAT_EQ(ps->bounce, 0.6f);
    EXPECT_EQ(ps->blendMode, Rendering::ParticleBlend::Additive);

    EXPECT_EQ(Eval<std::string>("lua_ps:GetBackend()"), "CPU");
    EXPECT_FALSE(Eval<bool>("lua_ps:GetPlayOnAwake()"));
    EXPECT_EQ(Eval<int>("lua_ps:GetMaxParticles()"), 250);
    EXPECT_EQ(Eval<int>("lua_ps:GetSeed()"), 17);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetEmissionRate()"), 12.5f);
    EXPECT_EQ(Eval<int>("lua_ps:GetBurstCount()"), 3);
    EXPECT_EQ(Eval<std::string>("lua_ps:GetShape()"), "Sphere");
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetShapeRadius()"), 0.75f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetConeAngle()"), 40.0f);
    EXPECT_FLOAT_EQ(Eval<float>("select(1, lua_ps:GetLifetime())"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_ps:GetLifetime())"), 2.5f);
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_ps:GetSpeed())"), 3.0f);
    EXPECT_FLOAT_EQ(Eval<float>("select(1, lua_ps:GetSize())"), 0.3f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetStartColor().r"), 1.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetStartColor().a"), 0.75f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetEndColor().b"), 1.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetGravityScale()"), 0.25f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetDrag()"), 0.5f);
    EXPECT_TRUE(Eval<bool>("(lua_ps:GetGround())"));
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_ps:GetGround())"), -1.5f);
    EXPECT_FLOAT_EQ(Eval<float>("select(3, lua_ps:GetGround())"), 0.6f);
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBlendMode()"), "Additive");
    EXPECT_TRUE(Eval<bool>("lua_fx:GetComponent('ParticleSystem') == lua_ps"));
}

TEST_F(LuaParticleSystemTest, TheDefaultsAreWhatTheComponentHas)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
    )");
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBackend()"), "Auto");
    EXPECT_TRUE(Eval<bool>("lua_ps:GetPlayOnAwake()"));
    EXPECT_EQ(Eval<int>("lua_ps:GetMaxParticles()"), 1000);
    EXPECT_EQ(Eval<std::string>("lua_ps:GetShape()"), "Cone");
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBlendMode()"), "Alpha");
    EXPECT_FALSE(Eval<bool>("lua_ps:IsPlaying()"));
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 0);
    EXPECT_TRUE(Eval<bool>("lua_ps:GetActiveBackend() == nil")) << "not started: no backend yet";
    EXPECT_TRUE(Eval<bool>("lua_ps:GetTexture() == nil"));
}

TEST_F(LuaParticleSystemTest, ThePbdMaterialPresetCanBeSetAndRead)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
    )");
    EXPECT_EQ(Eval<std::string>("lua_ps:GetPbdMaterial()"), "Water");
    Run(R"(lua_ps:SetPbdMaterial("Sand"))");
    EXPECT_EQ(Eval<std::string>("lua_ps:GetPbdMaterial()"), "Sand");
    const std::string error = RunExpectingError(R"(lua_ps:SetPbdMaterial("Lava"))");
    EXPECT_NE(error.find("Water, Sand, Smoke"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_ps:GetPbdMaterial()"), "Sand");
}

TEST_F(LuaParticleSystemTest, AnUnknownEnumNameIsAnErrorThatListsTheOptionsAndChangesNothing)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
    )");
    std::string error = RunExpectingError(R"(lua_ps:SetBackend("Gpu"))");
    EXPECT_NE(error.find("ParticleSystem:SetBackend"), std::string::npos) << error;
    EXPECT_NE(error.find("'Gpu'"), std::string::npos) << error;
    EXPECT_NE(error.find("Auto, PhysX, CPU"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBackend()"), "Auto");

    error = RunExpectingError(R"(lua_ps:SetShape("Box"))");
    EXPECT_NE(error.find("Cone, Sphere"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_ps:GetShape()"), "Cone");

    error = RunExpectingError(R"(lua_ps:SetBlendMode("Multiply"))");
    EXPECT_NE(error.find("Alpha, Additive"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBlendMode()"), "Alpha");
}

TEST_F(LuaParticleSystemTest, ATextureThatDoesNotLoadIsAnErrorAndNilClearsIt)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
    )");
    const std::string error = RunExpectingError(R"(lua_ps:SetTexture("res://no/such/spark.png"))");
    EXPECT_NE(error.find("ParticleSystem:SetTexture"), std::string::npos) << error;
    EXPECT_TRUE(Eval<bool>("lua_ps:GetTexture() == nil"));
    Run("lua_ps:SetTexture(nil)");
    EXPECT_TRUE(Eval<bool>("lua_ps:GetTexture() == nil"));
}

TEST_F(LuaParticleSystemTest, PlayStopClearAndEmitBurstOnAPlayingScene)
{
    PlayScene(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetPlayOnAwake(false)
        lua_ps:SetEmissionRate(100)
        lua_ps:SetLifetime(10, 10)
        SceneManager.GetCurrentScene():AddRootGameObject(lua_fx)
    )");
    Scene &scene = SceneManager::GetCurSceneRef();

    EXPECT_EQ(Eval<std::string>("lua_ps:GetActiveBackend()"), "CPU") << "started when the scene attached it";
    EXPECT_FALSE(Eval<bool>("lua_ps:IsPlaying()"));
    scene.FixedUpdate();
    scene.FixedUpdate();
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 0) << "not playing: no emission";

    Run("lua_ps:EmitBurst(6)");
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 6);

    Run("lua_ps:Play()");
    EXPECT_TRUE(Eval<bool>("lua_ps:IsPlaying()"));
    for (int i = 0; i < 10; ++i)
    {
        scene.FixedUpdate(); // 100 a second at 0.02 s: two a step
    }
    EXPECT_NEAR(Eval<int>("lua_ps:GetAliveCount()"), 26, 2);

    Run("lua_ps:Stop()");
    EXPECT_FALSE(Eval<bool>("lua_ps:IsPlaying()"));
    const int flying = Eval<int>("lua_ps:GetAliveCount()");
    for (int i = 0; i < 10; ++i)
    {
        scene.FixedUpdate();
    }
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), flying);

    Run("lua_ps:Clear()");
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 0);
}

TEST_F(LuaParticleSystemTest, PlayOnAwakeStartsWhenTheSceneAttachesItAndAPhysXRequestFallsBackToCpu)
{
    PlayScene(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetBackend("PhysX")
        lua_ps:SetEmissionRate(50)
        lua_ps:SetLifetime(10, 10)
        SceneManager.GetCurrentScene():AddRootGameObject(lua_fx)
    )");
    Scene &scene = SceneManager::GetCurSceneRef();
    EXPECT_TRUE(Eval<bool>("lua_ps:IsPlaying()"));
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBackend()"), "PhysX") << "what was asked for";
    EXPECT_EQ(Eval<std::string>("lua_ps:GetActiveBackend()"), "CPU") << "no GPU backend here: what is used";
    for (int i = 0; i < 25; ++i)
    {
        scene.FixedUpdate();
    }
    EXPECT_NEAR(Eval<int>("lua_ps:GetAliveCount()"), 25, 2);
}

TEST_F(LuaParticleSystemTest, ABurstAskedForBeforeTheSceneAttachesIsEmittedWhenItDoes)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetPlayOnAwake(false)
        lua_ps:EmitBurst(8)
    )");
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 0);

    SceneManager::AddScene(Scene::Create("LuaParticleSystemTest_Burst_" + std::to_string(++_scenes)), true);
    SceneManager::ProcessAnyPendingSceneChange();
    Run("SceneManager.GetCurrentScene():AddRootGameObject(lua_fx)");
    SceneManager::GetCurSceneRef().ProcessAttachQueue();
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 8);
}
