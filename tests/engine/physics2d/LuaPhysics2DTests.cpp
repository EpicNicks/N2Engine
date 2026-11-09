// 2D physics from Lua: what a script gets from the components, the events and the Physics2D table, and what happens to handles it
// keeps and to bad arguments. The behaviour itself is checked by Physics2DTests.cpp. With N2ENGINE_USE_BOX2D=OFF only the stripped
// tests are compiled.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Physics2D;
using namespace N2Engine::Scripting;
using Math::Vector2;
using Math::Vector3;
namespace fs = std::filesystem;

namespace
{
    class LuaPhysics2DFixture : public ::testing::Test
    {
    protected:
        static inline fs::path s_projectRoot;
        Scene *_scene = nullptr;

        static void SetUpTestSuite()
        {
            ASSERT_TRUE(LuaRuntime::Instance().Initialize());
            s_projectRoot = fs::temp_directory_path() / "n2engine_lua_physics2d_tests";
            std::error_code ec;
            fs::remove_all(s_projectRoot, ec);
            const fs::path script = s_projectRoot / "assets" / "p2d" / "Watcher.lua";
            fs::create_directories(script.parent_path());
            std::ofstream(script) << R"(
                local Watcher = {}
                Watcher.__index = Watcher

                function Watcher:OnCollisionEnter2D(collision)
                    p2d_enter = (p2d_enter or 0) + 1
                    p2d_collision = collision
                    local contact = collision:GetContact(1)
                    p2d_normal_y = contact and contact.normal.y
                    p2d_contacts = collision.contactCount
                end
                function Watcher:OnCollisionStay2D(collision) p2d_stay = (p2d_stay or 0) + 1 end
                function Watcher:OnCollisionExit2D(collision) p2d_exit = (p2d_exit or 0) + 1 end
                function Watcher:OnTriggerEnter2D(trigger)
                    p2d_trigger_enter = (p2d_trigger_enter or 0) + 1
                    p2d_trigger = trigger
                end
                function Watcher:OnTriggerStay2D(trigger) p2d_trigger_stay = (p2d_trigger_stay or 0) + 1 end
                function Watcher:OnTriggerExit2D(trigger) p2d_trigger_exit = (p2d_trigger_exit or 0) + 1 end

                return Watcher
            )";
            IO::ResourceLoader::Instance().Initialize(s_projectRoot);
        }

        void SetUp() override
        {
            Layers::ResetToDefaults();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
            SceneManager::AddScene(Scene::Create(std::string("LuaPhysics2D_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
            Run("p2d_enter, p2d_stay, p2d_exit, p2d_collision, p2d_normal_y, p2d_contacts = nil\n"
                "p2d_trigger_enter, p2d_trigger_stay, p2d_trigger_exit, p2d_trigger, kept = nil");
        }

        void TearDown() override
        {
            Run("p2d_collision, p2d_trigger, kept = nil");
            SceneManager::AddScene(Scene::Create("LuaPhysics2D_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
        }

        static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

        static void Run(const std::string &code)
        {
            const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error error = result;
                ADD_FAILURE() << "Lua error: " << error.what() << "\nin: " << code;
            }
        }

        template <typename T>
        static T Eval(const std::string &expression)
        {
            const sol::protected_function_result result = Lua().safe_script("return " + expression, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error error = result;
                ADD_FAILURE() << "Lua error: " << error.what() << "\nin: " << expression;
                return T{};
            }
            return result.template get<T>();
        }

        static std::string ErrorOf(const std::string &code)
        {
            const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
            if (result.valid())
            {
                return {};
            }
            const sol::error error = result;
            return error.what();
        }

        void Step(const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr Make(const std::string &name, const float x, const float y)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Vector3(x, y, 0.0f));
            _scene->AddRootGameObject(go);
            return go;
        }

        void Watch(const GameObject::Ptr &go)
        {
            go->AddComponent<LuaComponent>()->SetScript(IO::ResourcePath("res://p2d/Watcher.lua"));
        }
    };

#ifdef N2ENGINE_BOX2D_ENABLED

    using LuaPhysics2DTest = LuaPhysics2DFixture;

    TEST_F(LuaPhysics2DTest, AScriptBuildsABodyThatFallsOntoAFloor)
    {
        Run(R"(
            local scene = SceneManager.GetCurrentScene()
            local floor = GameObject.Create('Floor')
            floor:CreatePositionable()
            floor:GetPositionable():SetPosition(Vector3(0, -1, 0))
            scene:AddRootGameObject(floor)
            floor:AddComponent('BoxCollider2D'):SetHalfExtents(Vector2(50, 1))

            local crate = GameObject.Create('Crate')
            crate:CreatePositionable()
            crate:GetPositionable():SetPosition(Vector3(0, 2, 0))
            scene:AddRootGameObject(crate)
            kept_rb = crate:AddComponent('Rigidbody2D')
            kept_rb:SetBodyType(BodyType2D.Dynamic)
            crate:AddComponent('CircleCollider2D'):SetRadius(0.5)
        )");
        Step(200);

        const auto crate = _scene->FindGameObject("Crate");
        ASSERT_NE(crate, nullptr);
        EXPECT_NEAR(crate->GetPositionable()->GetPosition().y, 0.5f, 0.05f);
        EXPECT_NEAR(Eval<float>("kept_rb:GetGameObject():GetPositionable():GetPosition().y"), 0.5f, 0.05f);
        Run("kept_rb = nil");
    }

    TEST_F(LuaPhysics2DTest, EveryComponentMethodReachesTheComponent)
    {
        Run(R"(
            kept_go = GameObject.Create('Body')
            local go = kept_go
            local rb = go:AddComponent('Rigidbody2D')
            rb:SetBodyType(BodyType2D.Kinematic)
            assert(rb:GetBodyType() == BodyType2D.Kinematic)
            rb:SetBodyType(BodyType2D.Dynamic)
            rb:SetMass(4)
            assert(not rb:UsesAutoMass())
            assert(rb:GetMass() == 4)
            rb:UseAutoMass()
            assert(rb:UsesAutoMass())
            rb:SetLinearDamping(0.5)
            assert(rb:GetLinearDamping() == 0.5)
            rb:SetAngularDamping(0.25)
            assert(rb:GetAngularDamping() == 0.25)
            rb:SetGravityScale(0)
            assert(rb:GetGravityScale() == 0)
            rb:SetFixedRotation(true)
            assert(rb:IsFixedRotation())
            rb:SetBullet(true)
            assert(rb:IsBullet())
            rb:SetLinearVelocity(Vector2(1, 2))
            assert(rb:GetLinearVelocity().y == 2)
            rb:SetAngularVelocity(0.5)
            assert(rb:GetAngularVelocity() == 0.5)
            rb:AddForce(Vector2(1, 0))
            rb:AddImpulse(Vector2(1, 0))
            rb:AddTorque(1)
            rb:AddAngularImpulse(1)

            local box = go:AddComponent('BoxCollider2D')
            box:SetSize(Vector2(2, 4))
            assert(box:GetHalfExtents().y == 2)
            box:SetHalfExtents(Vector2(3, 3))
            assert(box:GetSize().x == 6)
            box:SetIsTrigger(true)
            assert(box:IsTrigger())
            box:SetOffset(Vector2(1, 1))
            assert(box:GetOffset().x == 1)
            box:SetDensity(2)
            assert(box:GetDensity() == 2)
            box:SetSurfaceSpeed(3)
            assert(box:GetSurfaceSpeed() == 3)
            box:SetMaterial(box:GetMaterial())

            local circle = go:AddComponent('CircleCollider2D')
            circle:SetRadius(2)
            assert(circle:GetRadius() == 2)

            local polygon = go:AddComponent('PolygonCollider2D')
            polygon:SetVertices({Vector2(0, 0), Vector2(2, 0), Vector2(0, 2)})
            local points = polygon:GetVertices()
            assert(#points == 3)
            assert(points[2].x == 2)

            assert(go:GetComponent('Rigidbody2D') == rb)
            assert(go:GetComponent('PolygonCollider2D') == polygon)
            kept_go = nil
        )");
        Step(2);
    }

    TEST_F(LuaPhysics2DTest, CollisionEventsReachAScriptWithItsData)
    {
        auto floor = Make("Floor", 0.0f, -1.0f);
        floor->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(50.0f, 1.0f));
        auto ball = Make("Ball", 0.0f, 1.0f);
        auto *rb = ball->AddComponent<Rigidbody2D>();
        ball->AddComponent<CircleCollider2D>();
        Watch(ball);

        Step(100);

        EXPECT_EQ(Eval<int>("p2d_enter"), 1);
        EXPECT_GT(Eval<int>("p2d_stay"), 1);
        EXPECT_GE(Eval<int>("p2d_contacts"), 1);
        EXPECT_NEAR(Eval<float>("p2d_normal_y"), 1.0f, 0.01f);
        EXPECT_EQ(Eval<std::string>("p2d_collision.gameObject:GetName()"), "Ball");
        EXPECT_EQ(Eval<std::string>("p2d_collision.otherGameObject:GetName()"), "Floor");
        EXPECT_TRUE(Eval<bool>("p2d_collision.rigidbody ~= nil"));
        EXPECT_TRUE(Eval<bool>("p2d_collision.otherRigidbody == nil"));
        EXPECT_TRUE(Eval<bool>("p2d_collision.collider:GetRadius() > 0"));
        EXPECT_TRUE(Eval<bool>("p2d_collision.otherCollider:GetHalfExtents().x == 50"));
        EXPECT_TRUE(Eval<bool>("p2d_collision:GetContact(99) == nil"));

        rb->SetLinearVelocity(Vector2(0.0f, 20.0f));
        Step(5);
        EXPECT_EQ(Eval<int>("p2d_exit"), 1);
    }

    TEST_F(LuaPhysics2DTest, TriggerEventsReachAScript)
    {
        auto zone = Make("Zone", 0.0f, 0.0f);
        zone->AddComponent<BoxCollider2D>()->SetIsTrigger(true);
        auto mover = Make("Mover", -3.0f, 0.0f);
        auto *rb = mover->AddComponent<Rigidbody2D>();
        rb->SetGravityScale(0.0f);
        rb->SetLinearVelocity(Vector2(5.0f, 0.0f));
        mover->AddComponent<BoxCollider2D>();
        Watch(mover);

        Step(100);

        EXPECT_EQ(Eval<int>("p2d_trigger_enter"), 1);
        EXPECT_GT(Eval<int>("p2d_trigger_stay"), 0);
        EXPECT_EQ(Eval<int>("p2d_trigger_exit"), 1);
        EXPECT_EQ(Eval<std::string>("p2d_trigger.otherGameObject:GetName()"), "Zone");
        EXPECT_TRUE(Eval<bool>("p2d_trigger.otherCollider:IsTrigger()"));
        EXPECT_TRUE(Eval<bool>("p2d_enter == nil"));
    }

    TEST_F(LuaPhysics2DTest, HandlesKeptPastTheCallbackAreCheckedAfterTheObjectIsGone)
    {
        auto floor = Make("Floor", 0.0f, -1.0f);
        floor->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(50.0f, 1.0f));
        auto ball = Make("Ball", 0.0f, 1.0f);
        ball->AddComponent<Rigidbody2D>();
        ball->AddComponent<CircleCollider2D>();
        Watch(ball);
        Step(60);
        ASSERT_EQ(Eval<int>("p2d_enter"), 1);
        Run("kept = p2d_collision.otherCollider; kept_go = p2d_collision.otherGameObject");
        ASSERT_TRUE(Eval<bool>("kept:IsValid()"));

        floor->Destroy();
        Step(2);
        floor.reset();

        EXPECT_FALSE(Eval<bool>("kept:IsValid()"));
        EXPECT_FALSE(ErrorOf("kept:GetHalfExtents()").empty()) << "a destroyed component must be a Lua error";
        EXPECT_FALSE(ErrorOf("kept_go:GetName()").empty());
        Run("kept_go = nil");
    }

    TEST_F(LuaPhysics2DTest, QueriesReturnTablesOrNil)
    {
        auto wall = Make("Wall", 5.0f, 0.0f);
        wall->AddComponent<BoxCollider2D>();
        auto zone = Make("Zone", 10.0f, 0.0f);
        zone->AddComponent<CircleCollider2D>()->SetIsTrigger(true);
        Step();

        EXPECT_NEAR(Eval<float>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0)).distance"), 4.5f, 0.001f);
        EXPECT_EQ(Eval<std::string>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0)).gameObject:GetName()"), "Wall");
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0)).rigidbody == nil"));
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0)).collider:GetSize().x == 1"));
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), 1) == nil"));
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(0, 0)) == nil"));
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), -1) == nil"));
        EXPECT_TRUE(Eval<bool>("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), nil, 0) == nil"));

        EXPECT_EQ(Eval<int>("#Physics2D.RaycastAll(Vector2(0, 0), Vector2(1, 0))"), 2);
        EXPECT_EQ(Eval<int>("#Physics2D.RaycastAll(Vector2(0, 0), Vector2(1, 0), nil, -1, true)"), 1) << "ignoreTriggers, and -1 is every layer";
        EXPECT_EQ(Eval<int>("#Physics2D.RaycastAll(Vector2(0, 50), Vector2(1, 0))"), 0);

        EXPECT_NEAR(Eval<float>("Physics2D.CircleCast(Vector2(0, 0), 0.5, Vector2(1, 0)).distance"), 4.0f, 0.01f);
        EXPECT_NEAR(Eval<float>("Physics2D.BoxCast(Vector2(0, 0), Vector2(1, 1), nil, Vector2(1, 0)).distance"), 3.5f, 0.01f);
        EXPECT_NEAR(Eval<float>("Physics2D.BoxCast(Vector2(0, 0), Vector2(1, 1), 0, Vector2(1, 0), 10).distance"), 3.5f, 0.01f);

        EXPECT_EQ(Eval<int>("#Physics2D.OverlapCircle(Vector2(5, 0), 1)"), 1);
        EXPECT_EQ(Eval<std::string>("Physics2D.OverlapCircle(Vector2(5, 0), 1)[1].gameObject:GetName()"), "Wall");
        EXPECT_EQ(Eval<int>("#Physics2D.OverlapCircle(Vector2(10, 0), 1, nil, true)"), 0);
        EXPECT_EQ(Eval<int>("#Physics2D.OverlapBox(Vector2(5, 0), Vector2(1, 1))"), 1);
        EXPECT_EQ(Eval<int>("#Physics2D.OverlapBox(Vector2(5, 0), Vector2(1, 1), 0.5, -1)"), 1);
    }

    TEST_F(LuaPhysics2DTest, GravityCanBeSetFromAScript)
    {
        Run("Physics2D.SetGravity(Vector2(0, 0))");
        EXPECT_FLOAT_EQ(Eval<float>("Physics2D.GetGravity().y"), 0.0f);
        auto go = Make("Floating", 0.0f, 0.0f);
        go->AddComponent<Rigidbody2D>();
        go->AddComponent<CircleCollider2D>();
        Step(20);
        EXPECT_FLOAT_EQ(go->GetPositionable()->GetPosition().y, 0.0f);
    }

    TEST_F(LuaPhysics2DTest, WrongArgumentsAreLuaErrors)
    {
        EXPECT_FALSE(ErrorOf("Physics2D.Raycast(1, Vector2(1, 0))").empty());
        EXPECT_NE(ErrorOf("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), 'far')").find("maxDistance"), std::string::npos);
        EXPECT_NE(ErrorOf("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), 1, 'layer')").find("layerMask"), std::string::npos);
        EXPECT_NE(ErrorOf("Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0), 1, 1, 'yes')").find("ignoreTriggers"), std::string::npos);
        EXPECT_NE(ErrorOf("Physics2D.OverlapBox(Vector2(0, 0), Vector2(1, 1), 'tilted')").find("angle"), std::string::npos);
        Run("kept_go = GameObject.Create('Poly'); kept = kept_go:AddComponent('PolygonCollider2D')");
        EXPECT_NE(ErrorOf("kept:SetVertices({Vector2(0, 0), 'corner'})").find("Vector2"), std::string::npos);
        Run("kept_go = nil");
    }

#else // A build without Box2D

    using LuaPhysics2DStrippedTest = LuaPhysics2DFixture;

    TEST_F(LuaPhysics2DStrippedTest, ScriptsKeepWorkingAndFindNothing)
    {
        Run(R"(
            kept_go = GameObject.Create('Body')
            local go = kept_go
            local rb = go:AddComponent('Rigidbody2D')
            rb:SetMass(4)
            assert(rb:GetMass() == 4)
            rb:SetLinearVelocity(Vector2(1, 0))
            rb:AddForce(Vector2(1, 0))
            local box = go:AddComponent('BoxCollider2D')
            box:SetSize(Vector2(2, 2))
            assert(box:GetSize().x == 2)
            assert(Physics2D.Raycast(Vector2(0, 0), Vector2(1, 0)) == nil)
            assert(#Physics2D.RaycastAll(Vector2(0, 0), Vector2(1, 0)) == 0)
            assert(#Physics2D.OverlapCircle(Vector2(0, 0), 5) == 0)
            kept_go = nil
        )");
        Step(2);
        EXPECT_EQ(_scene->GetPhysics2DWorld(), nullptr);
    }

#endif
}
