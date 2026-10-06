#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp" // ActionMap.hpp only forward-declares it; destroying a map needs the full type
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaJson.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/text/TextEffects.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/UIText.hpp"

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

    const auto go = Lua()["add_test_go"].get<GameObjectRef>().Lock();
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
             std::pair{"Smoke HUD", "res://scripts/SmokeHud.lua"},
             std::pair{"Material Textured Cube", "res://scripts/Spin.lua"},
             std::pair{"Robot Spinning", "res://scripts/Spin.lua"},
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

// ============================================================================
// The GPU smoke test's stations (docs/testing.html, "GPU smoke test"), built headless: CI can't see their pixels,
// but a broken asset, a missing binding or a script error in scene.lua fails here
// ============================================================================

namespace
{
    template <typename T>
    T *ComponentOf(const std::shared_ptr<GameObject> &go)
    {
        return go ? go->GetComponent<T>() : nullptr;
    }

    std::string UITextOf(const std::shared_ptr<GameObject> &go)
    {
        const auto *text = ComponentOf<UI::UIText>(go);
        return text ? text->GetText() : std::string("<no UIText>");
    }
}

TEST_F(LuaProjectSceneTest, SmokeWorldTextAndEffects)
{
    const auto *large = ComponentOf<Rendering::TextRenderer>(Find("Text Large"));
    ASSERT_NE(large, nullptr);
    EXPECT_FLOAT_EQ(large->GetFontSize(), 1.0f);
    const auto *smallText = ComponentOf<Rendering::TextRenderer>(Find("Text Small"));
    ASSERT_NE(smallText, nullptr);
    EXPECT_FLOAT_EQ(smallText->GetFontSize(), 0.25f);
    const auto *paragraph = ComponentOf<Rendering::TextRenderer>(Find("Text Paragraph"));
    ASSERT_NE(paragraph, nullptr);
    EXPECT_GT(paragraph->GetMaxWidth(), 0.0f);
    EXPECT_EQ(paragraph->GetHorizontalAlign(), Text::HorizontalAlign::Center);

    const auto *outline = ComponentOf<Rendering::TextRenderer>(Find("Effect Outline"));
    ASSERT_NE(outline, nullptr);
    EXPECT_FLOAT_EQ(outline->GetEffects().outlineWidth, 0.08f);

    const auto *shadow = ComponentOf<Rendering::TextRenderer>(Find("Effect Shadow"));
    ASSERT_NE(shadow, nullptr);
    EXPECT_FLOAT_EQ(shadow->GetEffects().shadowOffset.x, 0.08f);
    EXPECT_FLOAT_EQ(shadow->GetEffects().shadowOffset.y, -0.08f);
    EXPECT_GT(shadow->GetEffects().shadowColor.a, 0.0f) << "a transparent shadow draws nothing";

    const auto *glow = ComponentOf<Rendering::TextRenderer>(Find("Effect Glow"));
    ASSERT_NE(glow, nullptr);
    EXPECT_FLOAT_EQ(glow->GetEffects().shadowSoftness, 0.12f);

    const auto *softness = ComponentOf<Rendering::TextRenderer>(Find("Effect Softness"));
    ASSERT_NE(softness, nullptr);
    EXPECT_FLOAT_EQ(softness->GetEffects().softness, 0.12f);

    const auto *combined = ComponentOf<Rendering::TextRenderer>(Find("Effect Combined"));
    ASSERT_NE(combined, nullptr);
    const Text::TextEffects &effects = combined->GetEffects();
    EXPECT_GT(effects.outlineWidth, 0.0f);
    EXPECT_GT(effects.shadowSoftness, 0.0f);
    EXPECT_GT(effects.softness, 0.0f);
    // Inside the default font's spread (0.15 em), so drawing doesn't reduce them (docs/text.html, Text effects)
    EXPECT_LE(effects.outlineWidth + effects.softness / 2.0f, 0.15f);
    EXPECT_LE(effects.outlineWidth + effects.shadowSoftness / 2.0f + effects.shadowOffset.x, 0.15f);
}

TEST_F(LuaProjectSceneTest, SmokeWorldCanvasButtonCountsClicks)
{
    const auto *canvas = ComponentOf<UI::Canvas>(Find("Smoke World Canvas"));
    ASSERT_NE(canvas, nullptr);
    EXPECT_TRUE(canvas->IsWorldSpace());

    const auto *icon = ComponentOf<UI::Image>(Find("World Icon"));
    ASSERT_NE(icon, nullptr);
    ASSERT_NE(icon->GetSprite(), nullptr);
    EXPECT_TRUE(icon->GetSprite()->IsLoaded());

    auto *button = ComponentOf<UI::Button>(Find("World Button"));
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(UITextOf(Find("World Counter")), "World clicks: 0");
    EXPECT_TRUE(button->Click());
    EXPECT_EQ(UITextOf(Find("World Counter")), "World clicks: 1");
}

TEST_F(LuaProjectSceneTest, SmokeScreenUIButtonsCountAndToggle)
{
    const auto *canvas = ComponentOf<UI::Canvas>(Find("Smoke UI Panel"));
    ASSERT_NE(canvas, nullptr);
    EXPECT_FALSE(canvas->IsWorldSpace());

    const auto *icon = ComponentOf<UI::Image>(Find("UI Icon"));
    ASSERT_NE(icon, nullptr);
    ASSERT_NE(icon->GetSprite(), nullptr);
    EXPECT_TRUE(icon->GetSprite()->IsLoaded());

    const auto *wrapped = ComponentOf<UI::UIText>(Find("UI Wrap Text"));
    ASSERT_NE(wrapped, nullptr);
    EXPECT_TRUE(wrapped->GetWrap());
    const auto *bottomRight = ComponentOf<UI::UIText>(Find("UI Align Right / Bottom"));
    ASSERT_NE(bottomRight, nullptr);
    EXPECT_EQ(bottomRight->GetHorizontalAlign(), Text::HorizontalAlign::Right);
    EXPECT_EQ(bottomRight->GetVerticalAlign(), Text::VerticalAlign::Bottom);

    auto *button = ComponentOf<UI::Button>(Find("UI Button"));
    auto *toggle = ComponentOf<UI::Button>(Find("UI Toggle"));
    ASSERT_NE(button, nullptr);
    ASSERT_NE(toggle, nullptr);

    EXPECT_EQ(UITextOf(Find("UI Counter")), "Clicks: 0");
    EXPECT_TRUE(button->Click());
    EXPECT_EQ(UITextOf(Find("UI Counter")), "Clicks: 1");

    // The toggle disables the button, which then ignores clicks, and enables it again
    EXPECT_TRUE(toggle->Click());
    EXPECT_FALSE(button->IsInteractable());
    EXPECT_FALSE(button->Click());
    EXPECT_EQ(UITextOf(Find("UI Counter")), "Clicks: 1");
    EXPECT_TRUE(toggle->Click());
    EXPECT_TRUE(button->IsInteractable());
}

TEST_F(LuaProjectSceneTest, SmokeMaterialsComeFromTheirMatFiles)
{
    const auto *cube = ComponentOf<Rendering::MeshRenderer>(Find("Material Textured Cube"));
    ASSERT_NE(cube, nullptr);
    const auto cubeMaterial = cube->GetMaterial(0);
    ASSERT_NE(cubeMaterial, nullptr);
    EXPECT_EQ(cubeMaterial->GetShading(), Rendering::ShadingModel::Lit);
    ASSERT_NE(cubeMaterial->GetBaseColorTexture(), nullptr);
    EXPECT_TRUE(cubeMaterial->GetBaseColorTexture()->IsLoaded());

    for (const auto &[objectName, alphaMode] : {
             std::pair{"Material Opaque Quad", Rendering::AlphaMode::Opaque},
             std::pair{"Material Blend Quad", Rendering::AlphaMode::Blend},
             std::pair{"Material Mask Quad", Rendering::AlphaMode::Mask},
         })
    {
        const auto *quad = ComponentOf<Rendering::MeshRenderer>(Find(objectName));
        ASSERT_NE(quad, nullptr) << objectName;
        const auto material = quad->GetMaterial(0);
        ASSERT_NE(material, nullptr) << objectName;
        EXPECT_EQ(material->GetAlphaMode(), alphaMode) << objectName;
        ASSERT_NE(material->GetBaseColorTexture(), nullptr) << objectName;
        EXPECT_TRUE(material->GetBaseColorTexture()->IsLoaded()) << objectName;
    }

    const auto *lit = ComponentOf<Rendering::MeshRenderer>(Find("Material Lit Sphere"));
    const auto *unlit = ComponentOf<Rendering::MeshRenderer>(Find("Material Unlit Sphere"));
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(unlit, nullptr);
    ASSERT_NE(lit->GetMaterial(0), nullptr);
    ASSERT_NE(unlit->GetMaterial(0), nullptr);
    EXPECT_EQ(lit->GetMaterial(0)->GetShading(), Rendering::ShadingModel::Lit);
    EXPECT_EQ(unlit->GetMaterial(0)->GetShading(), Rendering::ShadingModel::Unlit);
}

TEST_F(LuaProjectSceneTest, SmokeModelIsInstantiatedWithItsHierarchy)
{
    for (const std::string holderName : {"Robot Static", "Robot Spinning"})
    {
        const auto holder = Find(holderName);
        ASSERT_NE(holder, nullptr) << holderName;
        const auto robot = holder->FindChild("smoke_robot");
        ASSERT_NE(robot, nullptr) << holderName;

        // smoke_robot > Robot > Body > (Arm.L, Arm.R, Head > Antenna)
        const auto body = robot->FindChildRecursive("Body");
        ASSERT_NE(body, nullptr) << holderName;
        for (const std::string part : {"Arm.L", "Arm.R", "Head"})
        {
            const auto child = body->FindChild(part);
            ASSERT_NE(child, nullptr) << holderName << " " << part;
            EXPECT_NE(child->GetComponent<Rendering::MeshRenderer>(), nullptr) << holderName << " " << part;
        }
        const auto head = body->FindChild("Head");
        ASSERT_NE(head->FindChild("Antenna"), nullptr) << holderName;

        const auto *bodyRenderer = body->GetComponent<Rendering::MeshRenderer>();
        ASSERT_NE(bodyRenderer, nullptr) << holderName;
        const auto bodyMaterial = bodyRenderer->GetMaterial(0);
        ASSERT_NE(bodyMaterial, nullptr) << holderName;
        ASSERT_NE(bodyMaterial->GetBaseColorTexture(), nullptr) << "the face texture: " << holderName;
        EXPECT_TRUE(bodyMaterial->GetBaseColorTexture()->IsLoaded()) << holderName;
    }
}

TEST_F(LuaProjectSceneTest, SmokeHudShowsTheFirstStation)
{
    const auto hud = Find("Smoke HUD");
    ASSERT_NE(hud, nullptr);
    auto *script = hud->GetComponent<LuaComponent>();
    ASSERT_NE(script, nullptr);
    script->OnAttach();

    EXPECT_EQ(UITextOf(Find("HUD Title")), "Station 1/6: Physics");
    EXPECT_FALSE(UITextOf(Find("HUD Caption")).empty());
}

// Every asset under lua_project/assets loads: each .mat with the texture it names, each image, and each model with
// no import warnings and all its meshes and textures. A new smoke asset is covered without a new test.
TEST_F(LuaProjectSceneTest, EveryProjectAssetLoads)
{
    namespace fs = std::filesystem;
    const fs::path assets = fs::path(N2_LUA_PROJECT_DIR) / "assets";
    auto &resources = IO::Resources::Instance();
    int checked = 0;
    for (const auto &entry : fs::recursive_directory_iterator(assets))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string resourcePath = "res://" + fs::relative(entry.path(), assets).generic_string();

        if (extension == ".mat")
        {
            const auto material = resources.Load<Rendering::Material>(fs::path(resourcePath));
            ASSERT_NE(material, nullptr) << resourcePath;
            std::ifstream file(entry.path());
            const json source = json::parse(file, nullptr, false);
            ASSERT_TRUE(source.is_object()) << resourcePath << " isn't a JSON object";
            if (source.contains("baseColorTexture") && source["baseColorTexture"].is_string())
            {
                ASSERT_NE(material->GetBaseColorTexture(), nullptr) << resourcePath;
                EXPECT_TRUE(material->GetBaseColorTexture()->IsLoaded()) << resourcePath;
            }
            ++checked;
        }
        else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
                 extension == ".bmp")
        {
            const auto texture = resources.Load<Rendering::Texture>(fs::path(resourcePath));
            ASSERT_NE(texture, nullptr) << resourcePath;
            EXPECT_TRUE(texture->IsLoaded()) << resourcePath;
            ++checked;
        }
        else if (extension == ".gltf" || extension == ".glb")
        {
            const auto model = resources.Load<Rendering::Model>(fs::path(resourcePath));
            ASSERT_NE(model, nullptr) << resourcePath;
            ASSERT_TRUE(model->IsLoaded()) << resourcePath;
            const auto &warnings = model->GetImportWarnings();
            EXPECT_TRUE(warnings.empty()) << resourcePath << ": " << (warnings.empty() ? "" : warnings.front());
            EXPECT_FALSE(model->GetMeshes().empty()) << resourcePath;
            for (const auto &mesh : model->GetMeshes())
            {
                EXPECT_NE(mesh, nullptr) << resourcePath;
            }
            for (const auto &texture : model->GetTextures())
            {
                ASSERT_NE(texture, nullptr) << resourcePath;
                EXPECT_TRUE(texture->IsLoaded()) << resourcePath;
            }
            ++checked;
        }
    }
    EXPECT_GE(checked, 10) << "the smoke test's six .mat files, three images and one model";
}

TEST_F(LuaProjectSceneTest, SmokeRobotModelHasItsMeshesMaterialsAndTexture)
{
    const auto model =
        IO::Resources::Instance().Load<Rendering::Model>(std::filesystem::path("res://models/smoke_robot.gltf"));
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(model->IsLoaded());
    EXPECT_EQ(model->GetMeshes().size(), 3u);    // Body, Limb, Head
    EXPECT_EQ(model->GetMaterials().size(), 3u); // Face (textured), Orange, Teal
    ASSERT_EQ(model->GetTextures().size(), 1u);
    ASSERT_NE(model->GetTextures()[0], nullptr);
    EXPECT_EQ(model->GetTextures()[0]->GetWidth(), 64u);
    EXPECT_EQ(model->GetNodes().size(), 6u);
}
