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
    EXPECT_NE(error.find("Auto, PhysX, CPU, WebGPU"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_ps:GetBackend()"), "Auto");

    error = RunExpectingError(R"(lua_ps:SetShape("Torus"))");
    EXPECT_NE(error.find("Cone, Sphere, Circle, Edge, Box"), std::string::npos) << error;
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

// ---------------------------------------------------------------- issue #445

TEST_F(LuaParticleSystemTest, ShapesRotationAndRenderModeAreSetAndRead)
{
    Run(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetShape("Box")
        lua_ps:SetEmitFromEdge(true)
        lua_ps:SetBoxSize(Vector3(1, 2, 3))
        lua_ps:SetStartRotation(-15, 30)
        lua_ps:SetAngularVelocity(90, 180)
        lua_ps:SetRenderMode("Stretched")
        lua_ps:SetStretch(3, 0.5)
    )");
    const auto fx = Lua()["lua_fx"].get<GameObjectRef>().Lock();
    ASSERT_NE(fx, nullptr);
    const auto *ps = fx->GetComponent<ParticleSystem>();
    ASSERT_NE(ps, nullptr);
    EXPECT_EQ(ps->shape, Particles::EmitterShape::Box);
    EXPECT_TRUE(ps->emitFromEdge);
    EXPECT_FLOAT_EQ(ps->boxSize.z, 3.0f);
    EXPECT_FLOAT_EQ(ps->startRotationMin, -15.0f);
    EXPECT_FLOAT_EQ(ps->startRotationMax, 30.0f);
    EXPECT_FLOAT_EQ(ps->angularVelocityMin, 90.0f);
    EXPECT_FLOAT_EQ(ps->angularVelocityMax, 180.0f);
    EXPECT_EQ(ps->renderMode, Particles::ParticleRenderMode::Stretched);
    EXPECT_FLOAT_EQ(ps->lengthScale, 3.0f);
    EXPECT_FLOAT_EQ(ps->speedScale, 0.5f);

    EXPECT_EQ(Eval<std::string>("lua_ps:GetShape()"), "Box");
    EXPECT_TRUE(Eval<bool>("lua_ps:GetEmitFromEdge()"));
    EXPECT_FLOAT_EQ(Eval<float>("lua_ps:GetBoxSize().y"), 2.0f);
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_ps:GetStartRotation())"), 30.0f);
    EXPECT_FLOAT_EQ(Eval<float>("select(1, lua_ps:GetAngularVelocity())"), 90.0f);
    EXPECT_EQ(Eval<std::string>("lua_ps:GetRenderMode()"), "Stretched");
    EXPECT_FLOAT_EQ(Eval<float>("select(2, lua_ps:GetStretch())"), 0.5f);
    for (const char *shape : {"Circle", "Edge", "Cone", "Sphere"})
    {
        Run(std::string("lua_ps:SetShape('") + shape + "')");
        EXPECT_EQ(Eval<std::string>("lua_ps:GetShape()"), shape);
    }
    for (const char *mode : {"HorizontalBillboard", "VerticalBillboard", "Billboard"})
    {
        Run(std::string("lua_ps:SetRenderMode('") + mode + "')");
        EXPECT_EQ(Eval<std::string>("lua_ps:GetRenderMode()"), mode);
    }
    const std::string error = RunExpectingError("lua_ps:SetRenderMode('Mesh')");
    EXPECT_NE(error.find("Stretched"), std::string::npos) << "the error lists the options: " << error;
    EXPECT_EQ(ps->renderMode, Particles::ParticleRenderMode::Billboard) << "and changes nothing";
}

TEST_F(LuaParticleSystemTest, KillsAndAMovedBurstOnAPlayingScene)
{
    PlayScene(R"(
        lua_fx = GameObject.Create("LuaFx")
        lua_ps = lua_fx:AddComponent("ParticleSystem")
        lua_ps:SetPlayOnAwake(false)
        lua_ps:SetLifetime(10, 10)
        lua_ps:SetSpeed(0, 0)
        lua_ps:SetShapeRadius(0)
        SceneManager.GetCurrentScene():AddRootGameObject(lua_fx)
    )");

    // Moved and burst in the same frame: the burst starts at the new position
    Run(R"(
        lua_ps:EmitBurst(4)
        lua_fx:GetPositionable():SetPosition(Vector3(5, 0, 2))
        lua_ps:EmitBurst(3)
    )");
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 7);
    EXPECT_EQ(Eval<int>("#lua_ps:GetParticles()"), 7);
    EXPECT_EQ(Eval<int>("#lua_ps:GetParticles(2)"), 2) << "at most the count asked for";
    EXPECT_EQ(Eval<int>("#lua_ps:GetParticles(-1)"), 0);
    Run(R"(
        lua_moved = 0
        for _, p in ipairs(lua_ps:GetParticles()) do
            if p.position.x == 5 and p.position.z == 2 and p.age == 0 and p.velocity.y == 0 and p.rotation == 0 then
                lua_moved = lua_moved + 1
            end
        end
    )");
    EXPECT_EQ(Eval<int>("lua_moved"), 3);

    // A circle in the XY plane around the new position takes the three (their z of 2 doesn't matter)
    EXPECT_EQ(Eval<int>("lua_ps:KillInCircle(Vector2(5, 0), 0.5)"), 3);
    EXPECT_EQ(Eval<int>("lua_ps:KillInCircle(Vector3(0, 0, 99), 0.5)"), 4) << "a Vector3 centre: its z is ignored";
    EXPECT_NE(RunExpectingError("lua_ps:KillInCircle(5, 0.5)").find("Vector2"), std::string::npos);
    Run("lua_ps:EmitBurst(2)");
    EXPECT_EQ(Eval<int>("lua_ps:KillInSphere(Vector3(5, 0, 2), 0.1)"), 2);
    Run("lua_ps:EmitBurst(2)");
    EXPECT_EQ(Eval<int>("lua_ps:KillInBox(Vector3(6, 1, 3), Vector3(4, -1, 1))"), 2);
    EXPECT_EQ(Eval<int>("lua_ps:GetAliveCount()"), 0);
    Run("lua_moved = nil");
}

TEST_F(LuaParticleSystemTest, SetsPerParticleCollision)
{
    // Issue #445
    Run(R"(
        local ps = GameObject.Create('LuaParticleCollision'):AddComponent('ParticleSystem')
        local a, b = ps:GetColliderCollision()
        assert(a == false and b == false)
        ps:SetColliderCollision(true, false)
        a, b = ps:GetColliderCollision()
        assert(a == true and b == false)
        ps:SetCollisionResponse(0.25, 0.5, 0.125, 1)
        local radius, bounce, dampen, loss = ps:GetCollisionResponse()
        assert(radius == 0.25 and bounce == 0.5 and dampen == 0.125 and loss == 1)
        assert(not pcall(function() ps:SetCollisionResponse(-1, 0, 0, 0) end))
        assert(not pcall(function() ps:SetCollisionResponse(0, 2, 0, 0) end))
        assert(select(1, ps:GetCollisionResponse()) == 0.25)
        ps:SetCollisionLayers(5)
        assert(ps:GetCollisionLayers() == 5)
        ps:SetCollisionLayers(0xFFFFFFFF)
        assert(ps:GetCollisionLayers() == 0xFFFFFFFF)
        assert(ps:GetCollisionCount() == 0 and #ps:GetCollisions() == 0)
    )");
}
