#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>
#include <math/Vector2.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// The pointer API as scripts see it: OnMouse* methods on a LuaComponent, Input's mouse functions, Ray and
// Camera:ScreenPointToRay, and Physics.Raycast without a backend (the PhysX side is in PickingPhysicsTests)
class LuaPickingTest : public ::testing::Test
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
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_picking_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // Appends each pointer event to a global, so the order is visible
        WriteAsset("picking/Clickable.lua", R"(
            local Clickable = {}
            Clickable.__index = Clickable

            local function Log(name) picking_log = picking_log .. name .. ";" end

            function Clickable:OnMouseEnter() Log("Enter") end
            function Clickable:OnMouseOver() Log("Over") end
            function Clickable:OnMouseExit() Log("Exit") end
            function Clickable:OnMouseDown() Log("Down") end
            function Clickable:OnMouseDrag() Log("Drag") end
            function Clickable:OnMouseUp() Log("Up") end
            function Clickable:OnMouseUpAsButton() Log("UpAsButton") end

            return Clickable
        )");

        // Only OnMouseDown: the rest are skipped, not errors
        WriteAsset("picking/DownOnly.lua", R"(
            local DownOnly = {}
            DownOnly.__index = DownOnly
            function DownOnly:OnMouseDown() picking_down_only = (picking_down_only or 0) + 1 end
            return DownOnly
        )");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

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

    Input::PointerDispatcher _dispatcher;
    Input::Mouse _mouse{nullptr};
    GameObject *_under = nullptr;

    void SetUp() override
    {
        _dispatcher.SetWorldHitProvider([this](const Math::Vector2 &) { return _under; });
    }

    void Frame(GameObject *under, const bool held)
    {
        _under = under;
        _mouse.InjectPointer(Math::Vector2(1.0f, 1.0f), held ? Input::Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(Input::PointerState::FromMouse(_mouse));
    }
};

TEST_F(LuaPickingTest, LuaComponentReceivesPointerEvents)
{
    const auto go = GameObject::Create("LuaClickable");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://picking/Clickable.lua"));
    ASSERT_FALSE(script->HasMissingScript());

    Run("picking_log = ''");
    Frame(go.get(), true);
    Frame(nullptr, true);
    Frame(go.get(), false);
    Frame(nullptr, false);
    EXPECT_EQ(Eval<std::string>("picking_log"),
              "Down;Enter;Over;"
              "Drag;Exit;"
              "UpAsButton;Up;Enter;Over;"
              "Exit;");
}

TEST_F(LuaPickingTest, MissingMethodsAreSkipped)
{
    const auto go = GameObject::Create("LuaDownOnly");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://picking/DownOnly.lua"));
    ASSERT_FALSE(script->HasMissingScript());

    Run("picking_down_only = 0");
    Frame(go.get(), true);
    Frame(go.get(), false);
    EXPECT_EQ(Eval<int>("picking_down_only"), 1);
}

TEST_F(LuaPickingTest, MouseFunctionsWithoutAWindow)
{
    // No window in the tests: no mouse, so the cursor reads (0, 0) and every button is up
    EXPECT_TRUE(Eval<bool>("Input.GetMousePosition().x == 0 and Input.GetMousePosition().y == 0"));
    EXPECT_FALSE(Eval<bool>("Input.GetMouseButton(0)"));
    EXPECT_FALSE(Eval<bool>("Input.GetMouseButtonDown(0)"));
    EXPECT_FALSE(Eval<bool>("Input.GetMouseButtonUp(1)"));
    EXPECT_FALSE(Eval<bool>("Input.IsPointerOverUI()"));
    EXPECT_TRUE(Eval<bool>("Mouse.Get() == nil"));
}

TEST_F(LuaPickingTest, MouseUsertypeReadsButtons)
{
    _mouse.InjectPointer(Math::Vector2(3.0f, 4.0f), Input::Mouse::ButtonBit(2));
    _mouse.Update();
    Lua()["picking_mouse"] = &_mouse;
    EXPECT_TRUE(Eval<bool>("picking_mouse:GetButton(2)"));
    EXPECT_TRUE(Eval<bool>("picking_mouse:GetButtonDown(2)"));
    EXPECT_FALSE(Eval<bool>("picking_mouse:GetButtonUp(2)"));
    EXPECT_FALSE(Eval<bool>("picking_mouse:GetButton(0)"));
    EXPECT_FLOAT_EQ(Eval<float>("picking_mouse:GetPosition().y"), 4.0f);
    Lua()["picking_mouse"] = sol::lua_nil;
}

TEST_F(LuaPickingTest, RayAndScreenPointToRay)
{
    EXPECT_FLOAT_EQ(Eval<float>("Ray(Vector3(1, 2, 3), Vector3(0, 0, -1)):GetPoint(2).z"), 1.0f);
    EXPECT_FLOAT_EQ(Eval<float>("Ray.new(Vector3(1, 2, 3), Vector3(0, 1, 0)).origin.x"), 1.0f);

    Camera camera;
    camera.SetOrthographic(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
    camera.SetPosition(Math::Vector3(0.0f, 0.0f, 10.0f));
    Lua()["picking_camera"] = &camera;
    EXPECT_NEAR(Eval<float>("picking_camera:ScreenPointToRay(0, 0, 800, 600).origin.x"), -4.0f, 1e-3f);
    EXPECT_NEAR(Eval<float>("picking_camera:ScreenPointToRay(0, 0, 800, 600).origin.y"), 3.0f, 1e-3f)
        << "window y is down";
    EXPECT_NEAR(Eval<float>("picking_camera:ScreenPointToRay(400, 300, 800, 600).direction.z"), -1.0f, 1e-3f);
    // Without a window the viewport is empty: the centre ray
    EXPECT_NEAR(Eval<float>("picking_camera:ScreenPointToRay(0, 0).origin.x"), 0.0f, 1e-3f);
    // A lone width (or height) is a mistake, not a request for the window's size
    EXPECT_FALSE(Lua().safe_script("return picking_camera:ScreenPointToRay(0, 0, 800)", sol::script_pass_on_error).valid());
    EXPECT_FALSE(
        Lua().safe_script("return picking_camera:ScreenPointToRay(0, 0, nil, 600)", sol::script_pass_on_error).valid());
    Lua()["picking_camera"] = sol::lua_nil;
}

TEST_F(LuaPickingTest, RaycastBindingsHandleBadInput)
{
    // Whatever backend is installed, these can't hit anything
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 0), Vector3(0, 0, 0)) == nil")) << "zero direction";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 0), Vector3(0, 0, -1), -5) == nil")) << "negative distance";
    EXPECT_TRUE(Eval<bool>("Physics.Raycast(Vector3(0, 0, 0), Vector3(0, 0, -1), 10, 0) == nil")) << "empty mask";
    EXPECT_EQ(Eval<int>("#Physics.RaycastAll(Vector3(0, 0, 0), Vector3(0, 0, 0))"), 0);
    EXPECT_EQ(Eval<std::string>("type(Physics.RaycastAll(Vector3(0, 0, 0), Vector3(1, 0, 0), 10, 0))"), "table");
}
