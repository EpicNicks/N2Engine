// The Lua binding of the character controller (netcode B5), run as TYPED tests over every backend this build has:
// go:AddComponent("CharacterController"), its settings (a number in range or a Lua error), SetInput, GetState and
// SetState (a copy a script may keep, a table checked field by field), Teleport, Step, and a destroyed controller.
// What the movement does is in CharacterControllerTests.cpp; these tests are about what a script gets back and what
// happens to bad arguments.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <cstdint>
#include <string>

#include "engine/scripting/LuaRuntime.hpp"

namespace N2PhysicsTests
{
    template <typename Tag>
    class LuaCharacterControllerTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        static void SetUpTestSuite()
        {
            PhysicsCoreFixture<Tag>::SetUpTestSuite();
            ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
        }

        static sol::state &Lua() { return Scripting::LuaRuntime::Instance().GetState(); }

        // Runs a chunk; a Lua error is a test failure
        static void Run(const std::string &code)
        {
            const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error err = result;
                ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << code;
            }
        }

        // The value of a Lua expression; a Lua error is a test failure (and the default value)
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
            return result.template get<T>();
        }

        // True when the chunk raises a Lua error
        static bool Raises(const std::string &code)
        {
            return !Lua().safe_script(code, sol::script_pass_on_error).valid();
        }

        // A 60 x 60 ground whose top is y = 0
        void SpawnGround()
        {
            const auto ground = this->Spawn("Ground", Vector3(0.0f, -0.5f, 0.0f));
            ground->template AddComponent<BoxCollider>()->SetSize(Vector3(60.0f, 1.0f, 60.0f));
        }

        // `character` and `cc`, in the current scene, starting at `position`; the next Step attaches it
        void SpawnCharacter(const std::string &position)
        {
            Run(R"(
                character = GameObject.Create("Character")
                cc = character:AddComponent("CharacterController")
                SceneManager.GetCurrentScene():AddRootGameObject(character)
            )");
            Run("cc:Teleport(" + position + ")");
            this->Step();
        }
    };

    TYPED_TEST_SUITE(LuaCharacterControllerTest, AllBackends, BackendNames);

    TYPED_TEST(LuaCharacterControllerTest, ItIsAddedAndHasTheDocumentedSettings)
    {
        this->SpawnCharacter("Vector3(0, 3, 0)");
        EXPECT_TRUE(this->template Eval<bool>("cc:IsValid()"));
        EXPECT_TRUE(this->template Eval<bool>("character:GetComponent('CharacterController') == cc"));
        EXPECT_NEAR(this->template Eval<float>("cc:GetRadius()"), 0.5f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetHeight()"), 2.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetStepOffset()"), 0.3f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetSlopeLimit()"), 45.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetSkinWidth()"), 0.02f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetJumpSpeed()"), 5.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetGravityScale()"), 1.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetMaxFallSpeed()"), 50.0f, 1e-6f);
        EXPECT_EQ(this->template Eval<std::int64_t>("cc:GetLayerMask()"), static_cast<std::int64_t>(0xFFFFFFFBu))
            << "every layer but Ignore Raycast";
    }

    TYPED_TEST(LuaCharacterControllerTest, SettingsTakeANumberInRangeAndNothingElse)
    {
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run(R"(
            cc:SetRadius(0.4)
            cc:SetHeight(1.6)
            cc:SetStepOffset(0.25)
            cc:SetSlopeLimit(50)
            cc:SetSkinWidth(0.03)
            cc:SetJumpSpeed(6)
            cc:SetGravityScale(2)
            cc:SetMaxFallSpeed(30)
            cc:SetLayerMask(-1)
        )");
        EXPECT_NEAR(this->template Eval<float>("cc:GetRadius()"), 0.4f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetHeight()"), 1.6f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetStepOffset()"), 0.25f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetSlopeLimit()"), 50.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetSkinWidth()"), 0.03f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetJumpSpeed()"), 6.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetGravityScale()"), 2.0f, 1e-6f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetMaxFallSpeed()"), 30.0f, 1e-6f);
        EXPECT_EQ(this->template Eval<std::int64_t>("cc:GetLayerMask()"), 0xFFFFFFFFll) << "-1 is every layer";

        for (const char *bad : {"cc:SetRadius('wide')", "cc:SetRadius(nil)", "cc:SetRadius(0/0)",
                                "cc:SetRadius(math.huge)", "cc:SetRadius(0)", "cc:SetRadius(-1)",
                                "cc:SetHeight(0)", "cc:SetStepOffset(-0.1)", "cc:SetSlopeLimit(90)",
                                "cc:SetSkinWidth(0)", "cc:SetJumpSpeed(-1)", "cc:SetGravityScale(1000)",
                                "cc:SetMaxFallSpeed(0)", "cc:SetLayerMask('all')", "cc:SetLayerMask(nil)",
                                "cc:SetLayerMask()"})
        {
            EXPECT_TRUE(this->Raises(bad)) << bad;
        }
        EXPECT_NEAR(this->template Eval<float>("cc:GetRadius()"), 0.4f, 1e-6f) << "a refused value changes nothing";
    }

    TYPED_TEST(LuaCharacterControllerTest, SetInputChecksItsArguments)
    {
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("cc:SetInput(Vector3(1, 0, 2))");
        EXPECT_NEAR(this->template Eval<float>("cc:GetInput().move.z"), 2.0f, 1e-6f);
        EXPECT_FALSE(this->template Eval<bool>("cc:GetInput().jump"));
        this->Run("cc:SetInput(Vector3(1, 0, 2), true)");
        EXPECT_TRUE(this->template Eval<bool>("cc:GetInput().jump"));
        this->Run("cc:SetInput(Vector3(1, 0, 2), nil)");
        EXPECT_FALSE(this->template Eval<bool>("cc:GetInput().jump"));

        for (const char *bad : {"cc:SetInput(1)", "cc:SetInput()", "cc:SetInput('x')", "cc:SetInput(Vector3(0/0, 0, 0))",
                                "cc:SetInput(Vector3(0, 0, math.huge))", "cc:SetInput(Vector3(1, 0, 0), 'yes')",
                                "cc:SetInput(Vector3(1, 0, 0), 1)"})
        {
            EXPECT_TRUE(this->Raises(bad)) << bad;
        }
        EXPECT_NEAR(this->template Eval<float>("cc:GetInput().move.z"), 2.0f, 1e-6f) << "a refused input changes nothing";
    }

    TYPED_TEST(LuaCharacterControllerTest, ItFallsLandsAndWalks)
    {
        this->SpawnGround();
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("for i = 1, 150 do cc:Step(1 / 60) end");
        EXPECT_TRUE(this->template Eval<bool>("cc:IsGrounded()"));
        EXPECT_NEAR(this->template Eval<float>("cc:GetPosition().y"), 1.02f, 0.05f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetState().groundNormal.y"), 1.0f, 1e-3f);

        this->Run("cc:SetInput(Vector3(3, 0, 0)) x0 = cc:GetPosition().x for i = 1, 60 do cc:Step(1 / 60) end");
        EXPECT_NEAR(this->template Eval<float>("cc:GetPosition().x - x0"), 3.0f, 0.03f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetVelocity().x"), 3.0f, 0.05f);

        // The object follows
        EXPECT_NEAR(this->template Eval<float>("character:GetPositionable():GetPosition().x"), this->template Eval<float>("cc:GetPosition().x"),
                    1e-4f);
    }

    TYPED_TEST(LuaCharacterControllerTest, AJumpIsOnePress)
    {
        this->SpawnGround();
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("for i = 1, 150 do cc:Step(1 / 60) end");
        ASSERT_TRUE(this->template Eval<bool>("cc:IsGrounded()"));

        this->Run("cc:SetInput(Vector3(0, 0, 0), true) cc:Step(1 / 60)");
        EXPECT_FALSE(this->template Eval<bool>("cc:IsGrounded()"));
        EXPECT_GT(this->template Eval<float>("cc:GetVelocity().y"), 4.0f);
        EXPECT_FALSE(this->template Eval<bool>("cc:GetInput().jump")) << "used up by the step";
        this->Run("for i = 1, 150 do cc:Step(1 / 60) end");
        EXPECT_TRUE(this->template Eval<bool>("cc:IsGrounded()")) << "one jump, then back on the ground";
    }

    TYPED_TEST(LuaCharacterControllerTest, GetStateIsACopyAndSetStateRestoresIt)
    {
        this->SpawnGround();
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("for i = 1, 150 do cc:Step(1 / 60) end saved = cc:GetState()");
        EXPECT_TRUE(this->template Eval<bool>("saved.grounded"));
        EXPECT_NEAR(this->template Eval<float>("saved.position.y"), 1.02f, 0.05f);

        // A copy: moving the character does not change it
        this->Run("cc:Teleport(Vector3(9, 9, 9))");
        EXPECT_NEAR(this->template Eval<float>("saved.position.y"), 1.02f, 0.05f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetPosition().x"), 9.0f, 0.0f);
        EXPECT_FALSE(this->template Eval<bool>("cc:IsGrounded()"));

        // The same future from the same state
        this->Run(R"(
            local function run()
                for i = 1, 40 do
                    cc:SetInput(Vector3(3, 0, 1), i == 5)
                    cc:Step(1 / 60)
                end
                return cc:GetPosition()
            end
            cc:SetState(saved)
            a = run()
            cc:SetState(saved)
            b = run()
        )");
        EXPECT_TRUE(this->template Eval<bool>("a.x == b.x and a.y == b.y and a.z == b.z"));
        EXPECT_GT(this->template Eval<float>("a.x"), 1.0f);

        // SetState moves the object, and the other fields default to rest
        this->Run("cc:SetState({position = Vector3(1, 7, 2)})");
        EXPECT_NEAR(this->template Eval<float>("character:GetPositionable():GetPosition().y"), 7.0f, 1e-5f);
        EXPECT_NEAR(this->template Eval<float>("cc:GetVelocity().y"), 0.0f, 0.0f);
        EXPECT_FALSE(this->template Eval<bool>("cc:IsGrounded()"));
        this->Run("cc:SetState({position = Vector3(1, 7, 2), velocity = Vector3(0, -3, 0), grounded = false, "
                  "groundNormal = Vector3(0, 1, 0)})");
        EXPECT_NEAR(this->template Eval<float>("cc:GetVelocity().y"), -3.0f, 0.0f);
    }

    TYPED_TEST(LuaCharacterControllerTest, SetStateAndTeleportCheckTheirArguments)
    {
        this->SpawnCharacter("Vector3(1, 2, 3)");
        for (const char *bad : {"cc:SetState(5)", "cc:SetState()", "cc:SetState({})", "cc:SetState({position = 1})",
                                "cc:SetState({position = Vector3(0/0, 0, 0)})",
                                "cc:SetState({position = Vector3(0, 0, 0), velocity = 3})",
                                "cc:SetState({position = Vector3(0, 0, 0), velocity = Vector3(0, math.huge, 0)})",
                                "cc:SetState({position = Vector3(0, 0, 0), grounded = 'yes'})",
                                "cc:SetState({position = Vector3(0, 0, 0), groundNormal = 1})",
                                "cc:Teleport()", "cc:Teleport(3)", "cc:Teleport(Vector3(0, 0/0, 0))"})
        {
            EXPECT_TRUE(this->Raises(bad)) << bad;
        }
        EXPECT_NEAR(this->template Eval<float>("cc:GetPosition().x"), 1.0f, 0.0f) << "refused calls changed nothing";
        EXPECT_NEAR(this->template Eval<float>("cc:GetPosition().z"), 3.0f, 0.0f);
    }

    TYPED_TEST(LuaCharacterControllerTest, StepChecksItsArgumentAndSaysWhetherItRan)
    {
        this->SpawnGround();
        this->SpawnCharacter("Vector3(0, 3, 0)");
        for (const char *bad : {"cc:Step(0)", "cc:Step(-1)", "cc:Step('a')", "cc:Step()", "cc:Step(0/0)",
                                "cc:Step(math.huge)"})
        {
            EXPECT_TRUE(this->Raises(bad)) << bad;
        }
        EXPECT_TRUE(this->template Eval<bool>("cc:Step(1 / 60)"));
    }

    TYPED_TEST(LuaCharacterControllerTest, TheLayerMaskDecidesWhatItStandsOn)
    {
        this->SpawnGround(); // layer 0
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("cc:SetLayerMask(256)"); // layer 8 only
        this->Run("for i = 1, 120 do cc:Step(1 / 60) end");
        EXPECT_LT(this->template Eval<float>("cc:GetPosition().y"), -3.0f) << "fell through the ground";
        this->Run("cc:SetLayerMask(1) cc:Teleport(Vector3(0, 3, 0)) for i = 1, 150 do cc:Step(1 / 60) end");
        EXPECT_TRUE(this->template Eval<bool>("cc:IsGrounded()"));
    }

    TYPED_TEST(LuaCharacterControllerTest, ADestroyedControllerRaisesAnErrorInsteadOfTouchingFreedMemory)
    {
        this->SpawnGround();
        this->SpawnCharacter("Vector3(0, 3, 0)");
        this->Run("character:Destroy()");
        this->Step();
        EXPECT_TRUE(this->template Eval<bool>("cc:IsDestroyed()"));
        EXPECT_TRUE(this->Raises("cc:Step(1 / 60)"));
        EXPECT_TRUE(this->Raises("cc:GetState()"));
        EXPECT_TRUE(this->Raises("cc:SetRadius(1)"));
    }
}

#endif // a physics backend is in this build
