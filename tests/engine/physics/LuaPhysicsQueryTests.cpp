// The Lua Physics query bindings (stage J5), run as TYPED tests over every backend this build has: Physics.SphereCast,
// CapsuleCast, OverlapSphere, OverlapBox and OverlapCapsule (and the argument rules Physics.Raycast shares with them).
// The hits themselves are checked by PhysicsQueryTests.cpp; these tests are about what a script gets back and what
// happens to bad arguments: a hit table or nil, an array of overlap tables, an empty result instead of an error for
// arguments that cannot hit, and a Lua error for arguments of the wrong type.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <string>

#include "engine/Layers.hpp"
#include "engine/scripting/LuaRuntime.hpp"

namespace N2PhysicsTests
{
    template <typename Tag>
    class LuaPhysicsQueryTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        static void SetUpTestSuite()
        {
            PhysicsCoreFixture<Tag>::SetUpTestSuite();
            ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
        }

        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const int layer = Layers::Default)
        {
            const auto go = this->Spawn(name, position);
            go->SetLayer(layer);
            go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            return go;
        }

        static sol::state &Lua() { return Scripting::LuaRuntime::Instance().GetState(); }

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

        // True when the expression raises a Lua error
        static bool Raises(const std::string &expression)
        {
            return !Lua().safe_script("return " + expression, sol::script_pass_on_error).valid();
        }
    };

    TYPED_TEST_SUITE(LuaPhysicsQueryTest, AllBackends, BackendNames);

    TYPED_TEST(LuaPhysicsQueryTest, SphereCastReturnsAHitTable)
    {
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).gameObject:GetName()"),
                  "Target");
        EXPECT_NEAR(this->template Eval<float>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).distance"),
                    4.0f, 0.01f) << "the distance of the sphere's centre";
        EXPECT_NEAR(this->template Eval<float>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).point.z"),
                    4.5f, 0.01f) << "the point is on the surface that was hit";
        EXPECT_NEAR(this->template Eval<float>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).normal.z"),
                    -1.0f, 0.01f);
        EXPECT_NEAR(this->template Eval<float>(
                        "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).collider:GetSize().x"),
                    1.0f, 0.01f) << "the collider comes back as its own type";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100).rigidbody == nil"));

        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1)) ~= nil"))
            << "the default distance is long enough";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), math.huge) ~= nil"));
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 1e30) ~= nil"));
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 3) == nil"))
            << "too short";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(3, 0, 0), 0.5, Vector3(0, 0, 1), 100) == nil")) << "beside";
    }

    TYPED_TEST(LuaPhysicsQueryTest, SphereCastAndCapsuleCastCheckTheirArguments)
    {
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100) ~= nil")) << "the setup must be hittable";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 0), 100) == nil"))
            << "zero direction";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0, Vector3(0, 0, 1), 100) == nil"))
            << "zero radius";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), -1, Vector3(0, 0, 1), 100) == nil"))
            << "negative radius";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0/0, Vector3(0, 0, 1), 100) == nil"))
            << "NaN radius";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), -1) == nil"))
            << "negative distance";
        EXPECT_TRUE(this->template Eval<bool>("Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 0/0) == nil"))
            << "NaN distance";

        const std::string capsule = "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.5, Vector3(0, 0, 1)";
        EXPECT_TRUE(this->template Eval<bool>(capsule + ", 100) ~= nil")) << "the setup must be hittable";
        EXPECT_NEAR(this->template Eval<float>(capsule + ", 100).distance"), 4.0f, 0.01f);
        EXPECT_EQ(this->template Eval<std::string>(capsule + ").gameObject:GetName()"), "Target")
            << "the default distance";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), 100) ~= nil"))
            << "equal end points are a sphere";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.5, Vector3(0, 0, 0), 100) == nil"))
            << "zero direction";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0, Vector3(0, 0, 1), 100) == nil")) << "zero radius";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.5, Vector3(0, 0, 1), -1) == nil"))
            << "negative distance";

        // Arguments of the wrong type are Lua errors, as for every other binding
        EXPECT_TRUE(this->Raises("Physics.SphereCast('x', 0.5, Vector3(0, 0, 1), 100)"));
        EXPECT_TRUE(this->Raises("Physics.SphereCast(Vector3(0, 0, 0), 'wide', Vector3(0, 0, 1), 100)"));
        EXPECT_TRUE(this->Raises("Physics.CapsuleCast(Vector3(0, 0, 0), 5, 0.5, Vector3(0, 0, 1), 100)"));
        EXPECT_TRUE(this->Raises("Physics.SphereCast(Vector3(0, 0, 0), 0.5)")) << "missing arguments";
    }

    TYPED_TEST(LuaPhysicsQueryTest, CastsTakeALayerMask)
    {
        this->SpawnBox("Nine", Vector3(0.0f, 0.0f, 5.0f), 9);
        this->SpawnBox("Hidden", Vector3(0.0f, 0.0f, 9.0f), Layers::IgnoreRaycast);
        this->Step();

        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), nil, Layers.MaskOf(9)).gameObject:GetName()"),
                  "Nine");
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), nil, Layers.MaskOf(10)) == nil"));
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), nil, 0) == nil")) << "an empty mask";
        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.SphereCast(Vector3(0, 0, 0), 0.5, Vector3(0, 0, 1), nil, Layers.MaskOf(Layers.IgnoreRaycast)).gameObject:GetName()"),
                  "Hidden");
        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.SphereCast(Vector3(0, 0, 7), 0.5, Vector3(0, 0, 1), nil, -1).gameObject:GetName()"),
                  "Hidden") << "-1 is every layer, Ignore Raycast included";
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.SphereCast(Vector3(0, 0, 7), 0.5, Vector3(0, 0, 1)) == nil"))
            << "the default mask leaves out Ignore Raycast";

        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.5, Vector3(0, 0, 1), nil, Layers.MaskOf(9)).gameObject:GetName()"),
                  "Nine");
        EXPECT_TRUE(this->template Eval<bool>(
            "Physics.CapsuleCast(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.5, Vector3(0, 0, 1), nil, 0) == nil"));
    }

    TYPED_TEST(LuaPhysicsQueryTest, OverlapsReturnArraysOfTables)
    {
        const auto first = this->SpawnBox("First", Vector3(0.0f, 0.0f, 0.0f));
        this->SpawnBox("Second", Vector3(3.0f, 0.0f, 0.0f));
        this->SpawnBox("Far", Vector3(10.0f, 0.0f, 0.0f));
        const auto body = this->Spawn("Body", Vector3(0.0f, 0.0f, 20.0f));
        body->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        this->AddFloatingBody(*body, 1.0f);
        this->Step();

        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(1.5, 0, 0), 2)"), 2);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(1.5, 0, 0), 0.2)"), 0);
        EXPECT_TRUE(this->template Eval<bool>("next(Physics.OverlapSphere(Vector3(1.5, 0, 0), 0.2)) == nil"))
            << "an empty table, not nil";
        EXPECT_EQ(this->template Eval<std::string>("Physics.OverlapSphere(Vector3(10, 0, 0), 0.2)[1].gameObject:GetName()"),
                  "Far");
        EXPECT_NEAR(this->template Eval<float>("Physics.OverlapSphere(Vector3(0, 0, 0), 0.2)[1].collider:GetSize().x"),
                    1.0f, 0.01f) << "the collider comes back as its own type";
        EXPECT_TRUE(this->template Eval<bool>("Physics.OverlapSphere(Vector3(0, 0, 0), 0.2)[1].rigidbody == nil"));
        EXPECT_TRUE(this->template Eval<bool>("Physics.OverlapSphere(Vector3(0, 0, 20), 0.2)[1].rigidbody ~= nil"))
            << "a dynamic body's Rigidbody";
        static_cast<void>(first);

        // Both of the first two, whichever comes first: the order is the backend's handle order
        EXPECT_TRUE(this->template Eval<bool>(R"((function()
            local names = {}
            for _, hit in ipairs(Physics.OverlapSphere(Vector3(1.5, 0, 0), 2)) do
                names[hit.gameObject:GetName()] = true
            end
            return names.First == true and names.Second == true
        end)())"));
    }

    TYPED_TEST(LuaPhysicsQueryTest, OverlapBoxTakesAnOptionalRotation)
    {
        this->SpawnBox("A", Vector3(3.0f, 0.0f, 0.0f));
        this->SpawnBox("B", Vector3(10.0f, 0.0f, 0.0f));
        this->Step();

        // Long along z at x = 7 it overlaps nothing; turned a quarter about y it reaches both
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(7, 0, 0), Vector3(0.2, 0.2, 5))"), 0);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(7, 0, 0), Vector3(0.2, 0.2, 5), nil)"), 0);
        EXPECT_EQ(this->template Eval<int>(
                      "#Physics.OverlapBox(Vector3(7, 0, 0), Vector3(0.2, 0.2, 5), Quaternion.Identity)"), 0);
        EXPECT_EQ(this->template Eval<int>(
                      "#Physics.OverlapBox(Vector3(7, 0, 0), Vector3(0.2, 0.2, 5), Quaternion(0.70710678, 0, 0.70710678, 0))"),
                  2);

        // The mask comes after the rotation, which can be nil
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(3, 0, 0), Vector3(1, 1, 1), nil, Layers.MaskOf(9))"),
                  0);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(3, 0, 0), Vector3(1, 1, 1), nil, -1)"), 1);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(3, 0, 0), Vector3(1, 1, 1))"), 1);
    }

    TYPED_TEST(LuaPhysicsQueryTest, OverlapCapsuleUsesItsSegment)
    {
        this->SpawnBox("Target", Vector3(3.0f, 0.0f, 0.0f));
        this->SpawnBox("Other", Vector3(8.0f, 0.0f, 0.0f));
        this->Step();

        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapCapsule(Vector3(3, -2, 0), Vector3(3, 2, 0), 0.5)"), 1);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapCapsule(Vector3(2, 0, 0), Vector3(8, 0, 0), 0.2)"), 2);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapCapsule(Vector3(3, 3, 0), Vector3(3, 5, 0), 0.5)"), 0);
        EXPECT_EQ(this->template Eval<int>(
                      "#Physics.OverlapCapsule(Vector3(2, 0, 0), Vector3(8, 0, 0), 0.2, Layers.MaskOf(9))"), 0);
        EXPECT_EQ(this->template Eval<std::string>(
                      "Physics.OverlapCapsule(Vector3(3, -2, 0), Vector3(3, 2, 0), 0.5)[1].gameObject:GetName()"),
                  "Target");
    }

    TYPED_TEST(LuaPhysicsQueryTest, OverlapsFindNothingForArgumentsThatCannotHit)
    {
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 0.0f), 4); // layer 4
        this->Step();

        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), 1, Layers.MaskOf(4))"), 1)
            << "the setup must be found";
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), 0, Layers.MaskOf(4))"), 0)
            << "zero radius";
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), -1, Layers.MaskOf(4))"), 0);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), 0/0, Layers.MaskOf(4))"), 0);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), 1, 0)"), 0) << "an empty mask";
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapSphere(Vector3(0, 0, 0), 1)"), 1) << "layer 4 is in the default mask";

        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(0, 0, 0), Vector3(1, 0, 1))"), 0) << "a flat box";
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapBox(Vector3(0, 0, 0), Vector3(1, -1, 1))"), 0);
        EXPECT_EQ(this->template Eval<int>(
                      "#Physics.OverlapBox(Vector3(0, 0, 0), Vector3(1, 1, 1), Quaternion(0, 0, 0, 0))"), 0)
            << "a rotation of no length";
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapCapsule(Vector3(0, 0, 0), Vector3(0, 1, 0), 0)"), 0);
        EXPECT_EQ(this->template Eval<int>("#Physics.OverlapCapsule(Vector3(0, 0, 0), Vector3(0, 1, 0), -2)"), 0);

        // Wrong types are Lua errors
        EXPECT_TRUE(this->Raises("Physics.OverlapSphere('here', 1)"));
        EXPECT_TRUE(this->Raises("Physics.OverlapSphere(Vector3(0, 0, 0), 'big')"));
        EXPECT_TRUE(this->Raises("Physics.OverlapBox(Vector3(0, 0, 0), 1)"));
        EXPECT_TRUE(this->Raises("Physics.OverlapBox(Vector3(0, 0, 0), Vector3(1, 1, 1), 'turned')"));
        EXPECT_TRUE(this->Raises("Physics.OverlapCapsule(Vector3(0, 0, 0), 2, 1)"));
        EXPECT_TRUE(this->Raises("Physics.OverlapSphere(Vector3(0, 0, 0))")) << "missing arguments";
    }
}

#endif // a physics backend is in this build
