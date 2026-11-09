#include <gtest/gtest.h>

#include <string>

#include <renderer/common/RenderState.hpp>

#include "engine/Application.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Screen.SetTint, Screen.ClearTint and Screen.GetTint: the screen tint of the scene pass (Application::SetScreenTint;
// effects design, stage E2). Every field is checked, an unknown one is an error, and a failed call changes nothing.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using Renderer::Common::ScreenTint;

namespace
{
    class LuaScreenTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }

        void SetUp() override { Application::GetInstance().ClearScreenTint(); }
        void TearDown() override { Application::GetInstance().ClearScreenTint(); }

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

        /// The error message of `code`, which must fail ("" and a test failure if it runs)
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

        static const ScreenTint &Tint() { return Application::GetInstance().GetScreenTint(); }
    };
}

TEST_F(LuaScreenTest, SetTintSetsEveryFieldAndClearTintRemovesIt)
{
    Run("Screen.SetTint{saturation = 0.2, centre = Vector2(0.25, 0.75), radius = 1.5, inside = false}");
    EXPECT_FLOAT_EQ(Tint().saturation, 0.2f);
    EXPECT_FLOAT_EQ(Tint().centreX, 0.25f);
    EXPECT_FLOAT_EQ(Tint().centreY, 0.75f);
    EXPECT_FLOAT_EQ(Tint().radius, 1.5f);
    EXPECT_FALSE(Tint().inside);
    EXPECT_TRUE(Tint().IsActive());

    Run("Screen.ClearTint()");
    EXPECT_EQ(Tint(), ScreenTint{});
    EXPECT_FALSE(Tint().IsActive());
}

TEST_F(LuaScreenTest, OmittedFieldsTakeTheirDefaultsNotThePreviousTintsValues)
{
    Run("Screen.SetTint{saturation = 0.5, centre = Vector2(0.1, 0.9), radius = 0.3, inside = false}");
    Run("Screen.SetTint{saturation = 0}");
    ScreenTint expected;
    expected.saturation = 0.0f;
    EXPECT_EQ(Tint(), expected) << "centre (0.5, 0.5), a radius covering the screen, inside";
    EXPECT_TRUE(Tint().CoversWholeTarget(1920.0f, 1080.0f)) << "full-screen grey";
}

TEST_F(LuaScreenTest, GetTintReturnsTheTintAsATable)
{
    EXPECT_FLOAT_EQ(Lua().safe_script("return Screen.GetTint().saturation").get<float>(), 1.0f) << "none: saturation 1";
    Run("Screen.SetTint{saturation = 0.25, centre = Vector2(0.3, 0.4), radius = 2, inside = true}");
    EXPECT_FLOAT_EQ(Lua().safe_script("return Screen.GetTint().saturation").get<float>(), 0.25f);
    EXPECT_FLOAT_EQ(Lua().safe_script("return Screen.GetTint().centre.x").get<float>(), 0.3f);
    EXPECT_FLOAT_EQ(Lua().safe_script("return Screen.GetTint().centre.y").get<float>(), 0.4f);
    EXPECT_FLOAT_EQ(Lua().safe_script("return Screen.GetTint().radius").get<float>(), 2.0f);
    EXPECT_TRUE(Lua().safe_script("return Screen.GetTint().inside").get<bool>());
}

TEST_F(LuaScreenTest, BadArgumentsAreErrorsAndChangeNothing)
{
    Run("Screen.SetTint{saturation = 0.4, radius = 1}");
    const ScreenTint before = Tint();

    const struct
    {
        const char *code;
        const char *message;
    } cases[] = {
        {"Screen.SetTint()", "takes a table"},
        {"Screen.SetTint(0.5)", "takes a table"},
        {"Screen.SetTint{}", "saturation is required"},
        {"Screen.SetTint{radius = 1}", "saturation is required"},
        {"Screen.SetTint{saturation = '0.5'}", "saturation must be a number"},
        {"Screen.SetTint{saturation = -0.1}", "saturation must be from 0"},
        {"Screen.SetTint{saturation = 1.5}", "saturation must be from 0"},
        {"Screen.SetTint{saturation = 0/0}", "saturation must be a finite number"},
        {"Screen.SetTint{saturation = 0, radius = -1}", "radius must be 0 or more"},
        {"Screen.SetTint{saturation = 0, radius = math.huge}", "radius must be a finite number"},
        {"Screen.SetTint{saturation = 0, radius = true}", "radius must be a number"},
        {"Screen.SetTint{saturation = 0, centre = {0.5, 0.5}}", "centre must be a Vector2"},
        {"Screen.SetTint{saturation = 0, centre = Vector3(0.5, 0.5, 0)}", "centre must be a Vector2"},
        {"Screen.SetTint{saturation = 0, centre = Vector2(0/0, 0.5)}", "centre must be a finite Vector2"},
        {"Screen.SetTint{saturation = 0, inside = 1}", "inside must be a boolean"},
        {"Screen.SetTint{saturation = 0, center = Vector2(0.5, 0.5)}", "unknown field 'center'"},
        {"Screen.SetTint{0.5}", "the keys must be field names"},
        {"Screen.ClearTint(1)", "takes no arguments"},
    };
    for (const auto &c : cases)
    {
        const std::string error = ErrorOf(c.code);
        EXPECT_NE(error.find(c.message), std::string::npos) << c.code << " gave: " << error;
        EXPECT_EQ(Tint(), before) << c.code << " changed the tint";
    }
}
