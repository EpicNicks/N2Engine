#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaJson.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
using json = nlohmann::json;

namespace
{
    sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    sol::object LuaValue(const std::string &expression)
    {
        return Lua().safe_script("return " + expression, sol::script_pass_on_error).get<sol::object>();
    }

    bool SameColor(const Common::Color &a, const Common::Color &b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
}

class LuaSceneTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

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

    // Runs a chunk that should fail and returns its error message
    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected a Lua error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
    }
};

// ============================================================================
// Lua table -> JSON
// ============================================================================

TEST_F(LuaSceneTest, LuaToJsonScalars)
{
    EXPECT_EQ(LuaToJson(LuaValue("true")), json(true));
    EXPECT_EQ(LuaToJson(LuaValue("'W'")), json("W"));
    EXPECT_EQ(LuaToJson(LuaValue("nil")), json(nullptr));
    EXPECT_EQ(LuaToJson(LuaValue("function() end")), json(nullptr));

    const json whole = LuaToJson(LuaValue("3"));
    EXPECT_TRUE(whole.is_number_integer());
    EXPECT_EQ(whole.get<int>(), 3);

    const json fractional = LuaToJson(LuaValue("0.25"));
    EXPECT_TRUE(fractional.is_number_float());
    EXPECT_DOUBLE_EQ(fractional.get<double>(), 0.25);
}

TEST_F(LuaSceneTest, LuaToJsonTables)
{
    EXPECT_EQ(LuaToJson(LuaValue("{ 'a', 'b', 'c' }")), json::array({"a", "b", "c"}));
    EXPECT_EQ(LuaToJson(LuaValue("{ type = 'KeyboardButton', key = 'Escape' }")),
              json({{"type", "KeyboardButton"}, {"key", "Escape"}}));
    EXPECT_EQ(LuaToJson(LuaValue("{}")), json::object());

    // A table with a gap isn't a sequence, so its keys become strings
    const json sparse = LuaToJson(LuaValue("{ [1] = 'x', [3] = 'y' }"));
    ASSERT_TRUE(sparse.is_object());
    EXPECT_EQ(sparse["1"], "x");
    EXPECT_EQ(sparse["3"], "y");

    const json nested = LuaToJson(LuaValue("{ outer = { inner = { 1, 2 } } }"));
    EXPECT_EQ(nested["outer"]["inner"], json::array({1, 2}));
}

// ============================================================================
// Action maps from Lua
// ============================================================================

TEST_F(LuaSceneTest, ActionMapJsonFromLuaKeepsBindingOrder)
{
    const sol::table actions = LuaValue(R"({
        ["Camera Move"] = {
            { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" },
            { type = "GamepadStick", xAxis = "LeftX", yAxis = "LeftY", deadzone = 0.25, invertY = true },
        },
        ["Quit"] = { { type = "KeyboardButton", key = "Escape" } },
    })").as<sol::table>();

    const json j = ActionMapJsonFromLua(actions);

    ASSERT_TRUE(j["actions"].is_object());
    const json &move = j["actions"]["Camera Move"]["bindings"];
    ASSERT_EQ(move.size(), 2u);
    EXPECT_EQ(move[0]["type"], "Vector2Composite");
    EXPECT_EQ(move[1]["type"], "GamepadStick");
    EXPECT_EQ(move[1]["invertY"], true);
    EXPECT_EQ(j["actions"]["Quit"]["bindings"][0]["key"], "Escape");
}

TEST_F(LuaSceneTest, ActionMapJsonFromLuaParsesAsActionMap)
{
    const sol::table actions = LuaValue(R"({
        ["Camera Move"] = {
            { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" },
            { type = "GamepadStick", xAxis = "LeftX", yAxis = "LeftY", deadzone = 0.25, invertY = true },
        },
        ["Camera Rotate"] = {
            { type = "Vector2Composite", up = "Up", down = "Down", left = "Left", right = "Right" },
        },
        ["Quit"] = { { type = "KeyboardButton", key = "Escape" } },
    })").as<sol::table>();

    // Bindings only use the window when polled, so a null window is fine for parsing
    auto result = Input::ActionMap::Deserialize(ActionMapJsonFromLua(actions), "Main Controls", nullptr);
    ASSERT_TRUE(result.has_value());

    const json roundTrip = result.value()->Serialize();
    ASSERT_EQ(roundTrip["actions"].size(), 3u);
    EXPECT_EQ(roundTrip["actions"]["Camera Move"]["bindings"].size(), 2u);
    EXPECT_EQ(roundTrip["actions"]["Camera Rotate"]["bindings"].size(), 1u);
    EXPECT_EQ(roundTrip["actions"]["Quit"]["bindings"][0]["type"], "KeyboardButton");
}

TEST_F(LuaSceneTest, CreateActionMapWithoutWindowReturnsNil)
{
    // Tests never open a window, so there's no input system to add the map to
    Run(R"(created_map = Input.CreateActionMap("No Window", { ["Quit"] = { { type = "KeyboardButton", key = "Escape" } } }))");

    EXPECT_FALSE(Lua()["created_map"].valid());
}

// ============================================================================
// GameObject:AddComponent / GetComponent
// ============================================================================

TEST_F(LuaSceneTest, AddComponentReturnsTypedComponent)
{
    Run(R"(
        add_test_go = GameObject.Create("AddComponentTest")
        add_test_go:AddComponent("CubeRenderer"):SetColor(Color.Green)
        add_test_go:AddComponent("BoxCollider"):SetSize(Vector3(2, 3, 4))
        add_test_go:AddComponent("Rigidbody"):SetBodyType(BodyType.Dynamic)
    )");

    const auto go = Lua()["add_test_go"].get<std::shared_ptr<GameObject>>();
    ASSERT_NE(go, nullptr);

    auto *cube = go->GetComponent<Example::CubeRenderer>();
    ASSERT_NE(cube, nullptr);
    EXPECT_TRUE(SameColor(cube->GetColor(), Common::Color::Green));

    auto *box = go->GetComponent<Physics::BoxCollider>();
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->GetSize().y, 3.0f);

    auto *body = go->GetComponent<Physics::Rigidbody>();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetBodyType(), Physics::BodyType::Dynamic);

    Lua()["add_test_go"] = sol::lua_nil;
}

TEST_F(LuaSceneTest, GetComponentReturnsSameComponentOrNil)
{
    Run(R"(
        get_test_go = GameObject.Create("GetComponentTest")
        get_test_added = get_test_go:AddComponent("SphereRenderer")
        get_test_added:SetRadius(2.5)
    )");

    EXPECT_FLOAT_EQ(LuaValue("get_test_go:GetComponent('SphereRenderer'):GetRadius()").as<float>(), 2.5f);
    EXPECT_TRUE(LuaValue("get_test_go:GetComponent('SphereRenderer') == get_test_added").as<bool>());
    EXPECT_TRUE(LuaValue("get_test_go:GetComponent('BoxCollider') == nil").as<bool>());

    Lua()["get_test_go"] = sol::lua_nil;
    Lua()["get_test_added"] = sol::lua_nil;
}

TEST_F(LuaSceneTest, EveryScriptableComponentCanBeAdded)
{
    for (const std::string &name : Bindings::GetScriptableComponentNames())
    {
        Run("local go = GameObject.Create('Every_" + name + "')\n"
            "local added = go:AddComponent('" + name + "')\n"
            "assert(added ~= nil, 'AddComponent returned nil')\n"
            "assert(go:GetComponent('" + name + "') ~= nil, 'GetComponent returned nil')");
    }
}

TEST_F(LuaSceneTest, UnknownComponentNameIsAnError)
{
    const std::string error = RunExpectingError("GameObject.Create('Unknown'):AddComponent('Teleporter')");

    EXPECT_NE(error.find("Unknown component type 'Teleporter'"), std::string::npos) << error;
    EXPECT_NE(error.find("BoxCollider"), std::string::npos) << "should list the known types: " << error;

    EXPECT_NE(RunExpectingError("GameObject.Create('Unknown'):GetComponent('Teleporter')")
                  .find("Unknown component type"),
              std::string::npos);
}

// ============================================================================
// lua_project/assets/scene.lua, built headless
// ============================================================================

class LuaProjectSceneTest : public LuaSceneTest
{
protected:
    static void SetUpTestSuite()
    {
        LuaSceneTest::SetUpTestSuite();

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(N2_LUA_PROJECT_DIR);

        SceneManager::AddScene(Scene::Create("LuaProjectSceneTest"), true);
        SceneManager::ProcessAnyPendingSceneChange();

        ASSERT_TRUE(LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua")));
    }

    static std::shared_ptr<GameObject> Find(const std::string &name)
    {
        return SceneManager::GetCurSceneRef().FindGameObject(name);
    }
};

TEST_F(LuaProjectSceneTest, RunFileReportsMissingScripts)
{
    EXPECT_FALSE(LuaRuntime::Instance().RunFile(IO::ResourcePath("res://does_not_exist.lua")));
}

TEST_F(LuaProjectSceneTest, BuildsFallingCube)
{
    const auto cube = Find("TestCube");
    ASSERT_NE(cube, nullptr);

    auto *renderer = cube->GetComponent<Example::CubeRenderer>();
    ASSERT_NE(renderer, nullptr);
    EXPECT_TRUE(SameColor(renderer->GetColor(), Common::Color::Blue));

    auto *collider = cube->GetComponent<Physics::BoxCollider>();
    ASSERT_NE(collider, nullptr);
    EXPECT_FLOAT_EQ(collider->GetSize().x, 1.0f);

    auto *body = cube->GetComponent<Physics::Rigidbody>();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetBodyType(), Physics::BodyType::Dynamic);
    EXPECT_TRUE(body->IsGravityEnabled());
}

TEST_F(LuaProjectSceneTest, BuildsFallingSphere)
{
    const auto sphere = Find("TestSphere");
    ASSERT_NE(sphere, nullptr);

    const Math::Vector3 pos = sphere->GetPositionable()->GetPosition();
    EXPECT_FLOAT_EQ(pos.x, 0.5f);
    EXPECT_FLOAT_EQ(pos.y, 4.0f);
    EXPECT_FLOAT_EQ(pos.z, 0.0f);

    auto *renderer = sphere->GetComponent<Example::SphereRenderer>();
    ASSERT_NE(renderer, nullptr);
    EXPECT_FLOAT_EQ(renderer->GetRadius(), 1.0f);
    EXPECT_TRUE(SameColor(renderer->GetColor(), Common::Color::Red));

    auto *collider = sphere->GetComponent<Physics::SphereCollider>();
    ASSERT_NE(collider, nullptr);
    EXPECT_FLOAT_EQ(collider->GetRadius(), 1.0f);

    auto *body = sphere->GetComponent<Physics::Rigidbody>();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetBodyType(), Physics::BodyType::Dynamic);
}

TEST_F(LuaProjectSceneTest, BuildsStaticFloor)
{
    const auto floor = Find("TestFloor");
    ASSERT_NE(floor, nullptr);

    EXPECT_FLOAT_EQ(floor->GetPositionable()->GetPosition().y, -5.0f);

    auto *renderer = floor->GetComponent<Example::CubeRenderer>();
    ASSERT_NE(renderer, nullptr);
    EXPECT_FLOAT_EQ(renderer->GetSize().x, 30.0f);
    EXPECT_FLOAT_EQ(renderer->GetSize().y, 1.0f);

    auto *collider = floor->GetComponent<Physics::BoxCollider>();
    ASSERT_NE(collider, nullptr);
    EXPECT_FLOAT_EQ(collider->GetSize().z, 30.0f);

    EXPECT_EQ(floor->GetComponent<Physics::Rigidbody>(), nullptr) << "the floor should be static";
}

TEST_F(LuaProjectSceneTest, BehaviourScriptsLoad)
{
    for (const auto &[objectName, scriptPath] : {
             std::pair{"Camera Controller", "res://scripts/CameraController.lua"},
             std::pair{"Quit Handler", "res://scripts/QuitHandler.lua"},
         })
    {
        const auto go = Find(objectName);
        ASSERT_NE(go, nullptr) << objectName;

        auto *script = go->GetComponent<LuaComponent>();
        ASSERT_NE(script, nullptr) << objectName;
        EXPECT_EQ(script->GetScriptPath().ToString(), scriptPath);
        EXPECT_FALSE(script->HasMissingScript()) << scriptPath;

        // No window, input system or camera in tests: the scripts must cope and not throw
        EXPECT_NO_THROW(script->OnAttach()) << scriptPath;
        EXPECT_NO_THROW(script->OnUpdate()) << scriptPath;
    }
}
