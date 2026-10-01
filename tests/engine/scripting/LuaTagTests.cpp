#include <gtest/gtest.h>

#include <string>

#include "engine/GameObjectScene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The tag API as scripts see it: GameObject:GetTag/SetTag/CompareTag and the Scene tag lookups
class LuaTagTest : public ::testing::Test
{
protected:
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
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

    static Scene &LoadFreshScene(const std::string &name)
    {
        SceneManager::AddScene(Scene::Create(name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return SceneManager::GetCurSceneRef();
    }
};

TEST_F(LuaTagTest, GameObjectTagMethods)
{
    Run("tag_go = GameObject('Tagged')");
    EXPECT_EQ(Eval<std::string>("tag_go:GetTag()"), "Untagged");

    Run("tag_go:SetTag('Player')");
    EXPECT_EQ(Eval<std::string>("tag_go:GetTag()"), "Player");
    EXPECT_TRUE(Eval<bool>("tag_go:CompareTag('Player')"));
    EXPECT_FALSE(Eval<bool>("tag_go:CompareTag('Enemy')"));

    Run("tag_go = nil");
}

TEST_F(LuaTagTest, SceneTagLookups)
{
    Scene &scene = LoadFreshScene("LuaTag_Scene");
    const auto first = GameObject::Create("FirstEnemy");
    const auto second = GameObject::Create("SecondEnemy");
    const auto plain = GameObject::Create("Plain");
    first->SetTag("Enemy");
    second->SetTag("Enemy");
    scene.AddRootGameObjects({first, plain, second});

    Run("tag_scene = SceneManager.GetCurrentScene()");
    EXPECT_EQ(Eval<std::string>("tag_scene:FindGameObjectWithTag('Enemy'):GetName()"), "FirstEnemy");
    EXPECT_TRUE(Eval<bool>("tag_scene:FindGameObjectWithTag('Missing') == nil"));
    EXPECT_EQ(Eval<int>("#tag_scene:FindGameObjectsWithTag('Enemy')"), 2);
    EXPECT_EQ(Eval<std::string>("tag_scene:FindGameObjectsWithTag('Enemy')[2]:GetName()"), "SecondEnemy");
    EXPECT_EQ(Eval<int>("#tag_scene:FindGameObjectsByTag('Enemy')"), 2);
    EXPECT_EQ(Eval<int>("#tag_scene:FindGameObjectsWithTag('Missing')"), 0);

    // A tag set from Lua is what the C++ side sees
    Run("tag_scene:FindGameObject('Plain'):SetTag('Pickup')");
    EXPECT_TRUE(plain->CompareTag("Pickup"));

    Run("tag_scene = nil");
}
