#include <gtest/gtest.h>

#include <string>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/rendering/RenderTexture.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// The Lua side of issue #445 (docs/post-processing.html): the PostProcessStack component's Set{...} and Get(), RenderTexture.new,
// Sprite2D:SetRenderTexture, Camera:RenderToTexture and Camera:SetTargetTexture, and Graphics.CanPostProcess. Every field is
// type-checked and an unknown one is an error.

using namespace N2Engine;
using namespace N2Engine::Scripting;

namespace
{
    class LuaPostProcessTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }
        void SetUp() override { Application::GetInstance().ClearPendingCaptures(); }
        void TearDown() override
        {
            Application::GetInstance().ClearPendingCaptures();
            Lua()["post_test_camera"] = sol::lua_nil;
            Run("collectgarbage()");
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

        static std::string ErrorOf(const std::string &code)
        {
            const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
            if (result.valid())
            {
                ADD_FAILURE() << "expected an error from: " << code;
                return "";
            }
            const sol::error err = result;
            return err.what();
        }
    };
}

TEST_F(LuaPostProcessTest, SetChangesTheFieldsGivenAndGetReturnsThemAll)
{
    Run(R"(
        local go = GameObject.Create('LuaPostProcess')
        local stack = go:AddComponent('PostProcessStack')
        local before = stack:Get()
        assert(before.bloom == false and before.bloomIterations == 4 and before.invert == 0)
        stack:Set{bloom = true, bloomThreshold = 0.5, bloomIntensity = 2, bloomIterations = 6, bloomTint = Color.new(1, 0.5, 0.25, 1),
                  invert = 0.25, chromaticAberration = 0.01, vignette = 0.5}
        local after = stack:Get()
        assert(after.bloom == true)
        assert(after.bloomThreshold == 0.5 and after.bloomIntensity == 2 and after.bloomIterations == 6)
        assert(math.abs(after.bloomTint.g - 0.5) < 1e-6 and math.abs(after.bloomTint.b - 0.25) < 1e-6)
        assert(after.invert == 0.25 and math.abs(after.chromaticAberration - 0.01) < 1e-6 and after.vignette == 0.5)
        stack:Set{invert = 0}
        assert(stack:Get().bloom == true and stack:Get().invert == 0)
        assert(go:GetComponent('PostProcessStack') == stack)
    )");
}

TEST_F(LuaPostProcessTest, SetsTheBlur)
{
    // Issue #445
    Run(R"(
        local stack = GameObject.Create('LuaPostProcessBlur'):AddComponent('PostProcessStack')
        local before = stack:Get()
        assert(before.blur == false and before.blurStrength == 1 and before.blurIterations == 3)
        stack:Set{blur = true, blurStrength = 0.5, blurIterations = 6}
        local after = stack:Get()
        assert(after.blur == true and after.blurStrength == 0.5 and after.blurIterations == 6)
        assert(not pcall(function() stack:Set{blur = 1} end))
        assert(not pcall(function() stack:Set{blurIterations = 1.5} end))
        assert(not pcall(function() stack:Set{blurStrength = 'a lot'} end))
        assert(stack:Get().blurIterations == 6)
    )");
}

TEST_F(LuaPostProcessTest, BadFieldsAreErrorsAndChangeNothing)
{
    Run("post_stack = GameObject.Create('LuaPostProcessErrors'):AddComponent('PostProcessStack')");
    EXPECT_NE(ErrorOf("post_stack:Set{blom = true}").find("unknown field 'blom'"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set{bloom = 1}").find("bloom must be a boolean"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set{invert = 'all'}").find("invert must be a number"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set{bloomIterations = 2.5}").find("whole number"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set{bloomTint = 3}").find("bloomTint must be a Color"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set(true)").find("takes a table"), std::string::npos);
    EXPECT_NE(ErrorOf("post_stack:Set{vignette = 0.5, bogus = 1}").find("unknown field 'bogus'"), std::string::npos);
    Run("assert(post_stack:Get().vignette == 0, 'a failed Set changed a field')");
    Run("post_stack = nil");
}

TEST_F(LuaPostProcessTest, RenderTexturesAreMadeShownAndCaptured)
{
    Camera camera;
    Lua()["post_test_camera"] = &camera;
    Run(R"(
        local rt = RenderTexture.new(64, 32)
        assert(rt:GetWidth() == 64 and rt:GetHeight() == 32 and rt:GetCaptureCount() == 0)
        local sprite = GameObject.Create('LuaRenderTextureSprite'):AddComponent('Sprite2D')
        sprite:SetRenderTexture(rt)
        assert(sprite:HasTexture())
        sprite:SetRenderTexture(nil)
        assert(not sprite:HasTexture())
        post_test_camera:RenderToTexture(rt)
        post_test_camera:SetTargetTexture(rt)
        assert(post_test_camera:GetTargetTexture() ~= nil)
        post_test_camera:SetTargetTexture(nil)
        assert(post_test_camera:GetTargetTexture() == nil)
        assert(type(Graphics.CanPostProcess()) == 'boolean')
    )");
    EXPECT_EQ(Application::GetInstance().GetPendingCaptureCount(), 1u);
    EXPECT_EQ(camera.GetTargetTexture(), nullptr);
}

TEST_F(LuaPostProcessTest, BadRenderTextureSizesAreErrors)
{
    EXPECT_NE(ErrorOf("RenderTexture.new(0, 4)").find("width must be a whole number"), std::string::npos);
    EXPECT_NE(ErrorOf("RenderTexture.new(4, 1.5)").find("height must be a whole number"), std::string::npos);
    EXPECT_NE(ErrorOf("RenderTexture.new(4, 100000)").find("height"), std::string::npos);
}
