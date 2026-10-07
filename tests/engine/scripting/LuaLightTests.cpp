#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Light from Lua: go:AddComponent("Light") and its type, direction, colour, intensity and range, as the scene's
// lighting collects them

using namespace N2Engine;
using namespace N2Engine::Scripting;

class LuaLightTest : public ::testing::Test
{
protected:
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    void TearDown() override
    {
        Run("lua_lamp = nil; lua_light = nil");
        Lua().collect_garbage();
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
};

TEST_F(LuaLightTest, SetsTheLightsFields)
{
    Run(R"(
        lua_lamp = GameObject.Create("LuaLamp")
        lua_light = lua_lamp:AddComponent("Light")
        lua_light:SetType("Point")
        lua_light:SetDirection(Vector3(0.4, -0.6, -0.7))
        lua_light:SetColor(Color.new(1.0, 0.5, 0.25, 0.1))
        lua_light:SetIntensity(1.5)
        lua_light:SetRange(20)
    )");

    const auto lamp = Lua()["lua_lamp"].get<GameObjectRef>().Lock();
    ASSERT_NE(lamp, nullptr);
    const auto *light = lamp->GetComponent<Rendering::Light>();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type, Rendering::LightType::Point);
    EXPECT_FLOAT_EQ(light->direction.x, 0.4f);
    EXPECT_FLOAT_EQ(light->direction.y, -0.6f);
    EXPECT_FLOAT_EQ(light->direction.z, -0.7f);
    EXPECT_FLOAT_EQ(light->color.x, 1.0f);
    EXPECT_FLOAT_EQ(light->color.y, 0.5f);
    EXPECT_FLOAT_EQ(light->color.z, 0.25f);
    EXPECT_FLOAT_EQ(light->intensity, 1.5f);
    EXPECT_FLOAT_EQ(light->range, 20.0f);

    EXPECT_EQ(Eval<std::string>("lua_light:GetType()"), "Point");
    EXPECT_FLOAT_EQ(Eval<float>("lua_light:GetDirection().z"), -0.7f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_light:GetColor().g"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_light:GetColor().a"), 1.0f) << "the alpha given is ignored";
    EXPECT_FLOAT_EQ(Eval<float>("lua_light:GetIntensity()"), 1.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_light:GetRange()"), 20.0f);
    EXPECT_TRUE(Eval<bool>("lua_lamp:GetComponent('Light') == lua_light"));
}

TEST_F(LuaLightTest, AnUnknownTypeIsAnErrorAndChangesNothing)
{
    Run(R"(
        lua_lamp = GameObject.Create("LuaLamp")
        lua_light = lua_lamp:AddComponent("Light")
    )");
    const std::string error = RunExpectingError(R"(lua_light:SetType("Area"))");
    EXPECT_NE(error.find("unknown light type 'Area'"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_light:GetType()"), "Directional");
}

TEST_F(LuaLightTest, ADirectionalLightFromLuaLightsTheScene)
{
    SceneManager::AddScene(Scene::Create("LuaLightTest_Scene"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    Scene &scene = SceneManager::GetCurSceneRef();

    Run(R"(
        lua_lamp = GameObject.Create("LuaSun")
        lua_light = lua_lamp:AddComponent("Light")
        lua_light:SetDirection(Vector3(0.0, -3.0, -4.0))
        lua_light:SetIntensity(0.9)
        SceneManager.GetCurrentScene():AddRootGameObject(lua_lamp)
    )");
    scene.ProcessAttachQueue();

    const auto lighting = scene.CollectLighting();
    ASSERT_EQ(lighting.directionalLights.size(), 1u) << "this light, not the default one";
    const auto &sun = lighting.directionalLights[0];
    EXPECT_NEAR(sun.direction.x, 0.0f, 1e-5f);
    EXPECT_NEAR(sun.direction.y, -0.6f, 1e-5f) << "normalized";
    EXPECT_NEAR(sun.direction.z, -0.8f, 1e-5f);
    EXPECT_FLOAT_EQ(sun.intensity, 0.9f);

    SceneManager::AddScene(Scene::Create("LuaLightTest_Empty"), true);
    SceneManager::ProcessAnyPendingSceneChange();
}
