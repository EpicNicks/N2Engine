#include <gtest/gtest.h>

#include <string>

#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The layer API as scripts see it: GameObject:GetLayer/SetLayer/SetLayerRecursive and the Layers table
class LuaLayerTest : public ::testing::Test
{
protected:
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    void SetUp() override { Layers::ResetToDefaults(); }
    void TearDown() override { Layers::ResetToDefaults(); }

    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    static bool Fails(const std::string &code)
    {
        return !Lua().safe_script(code, sol::script_pass_on_error).valid();
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

TEST_F(LuaLayerTest, GameObjectLayerMethods)
{
    SceneManager::AddScene(Scene::Create("LuaLayer_Scene"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    const auto parent = GameObject::Create("LayeredParent");
    const auto child = GameObject::Create("LayeredChild");
    parent->AddChild(child);
    SceneManager::GetCurSceneRef().AddRootGameObject(parent);

    Run("layer_go = SceneManager.GetCurrentScene():FindGameObject('LayeredParent')");
    EXPECT_EQ(Eval<int>("layer_go:GetLayer()"), Layers::Default);

    Run("layer_go:SetLayer(9)");
    EXPECT_EQ(parent->GetLayer(), 9) << "a layer set from Lua is what the C++ side sees";
    EXPECT_EQ(child->GetLayer(), Layers::Default);

    Run("layer_go:SetLayer(50)");
    EXPECT_EQ(Eval<int>("layer_go:GetLayer()"), 31) << "out of range is clamped";

    Run("layer_go:SetLayerRecursive(Layers.UI)");
    EXPECT_EQ(child->GetLayer(), Layers::UI);

    Run("layer_go = nil");
}

TEST_F(LuaLayerTest, NamesAndMasks)
{
    EXPECT_EQ(Eval<int>("Layers.NameToLayer('Water')"), Layers::Water);
    EXPECT_EQ(Eval<int>("Layers.NameToLayer('Nope')"), -1);
    EXPECT_EQ(Eval<std::string>("Layers.LayerToName(2)"), "Ignore Raycast");
    EXPECT_EQ(Eval<int>("Layers.IgnoreRaycast"), Layers::IgnoreRaycast);
    EXPECT_TRUE(Eval<bool>("Layers.DefaultRaycastMask == 4294967291"));
    EXPECT_TRUE(Eval<bool>("Layers.AllLayers == 4294967295"));
    EXPECT_TRUE(Eval<bool>("Layers.MaskOf(8) == 256"));
    EXPECT_TRUE(Eval<bool>("Layers.MaskOf(40) == 0"));

    EXPECT_TRUE(Eval<bool>("Layers.SetName(8, 'Enemy')"));
    EXPECT_EQ(Layers::NameToLayer("Enemy"), 8);
    EXPECT_FALSE(Eval<bool>("Layers.SetName(0, 'Ground')"));

    EXPECT_TRUE(Eval<bool>("Layers.GetMask('Default', 'Water') == 17"));
    EXPECT_TRUE(Eval<bool>("Layers.GetMask('Enemy', 'Nope') == 256"));
    EXPECT_TRUE(Eval<bool>("Layers.GetMask() == 0"));
    EXPECT_TRUE(Fails("return Layers.GetMask(5)")) << "a mask is built from layer names";
}

TEST_F(LuaLayerTest, CollisionMatrix)
{
    EXPECT_TRUE(Eval<bool>("Layers.GetCollision(8, 9)"));

    Run("Layers.SetCollision(8, 9, false)");
    EXPECT_FALSE(Layers::GetCollision(9, 8));
    EXPECT_FALSE(Eval<bool>("Layers.GetCollision(9, 8)"));

    Run("Layers.SetCollision(9, 8, true)");
    EXPECT_TRUE(Layers::GetCollision(8, 9));
}
