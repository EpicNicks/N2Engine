// The joint component (stage a of #381 item 6): the PhysicsFeature::Joints bit (only a backend that has joints reports it,
// and it is not one of the groups the Deterministic preset requires), a backend without joints degrading with one
// warning, and on Jolt each joint type constraining the way it should: Fixed, Hinge, Ball, Slider and Distance, with and
// without limits, the joint's lifetime (the component, either body, a disabled body), collideConnected, the component's
// serialization and its Lua binding. The backend-level and component behaviour tests are typed: they run on every physics
// backend this build has (PhysX and Jolt), so both must behave the same (the Lua tests run on one of them).

#include "PhysicsTestFixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../particles/FakePhysicsBackend.hpp"
#include "engine/Logger.hpp"
#include "engine/physics/Joint.hpp"
#include "engine/physics/PhysicsFeatures.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace
{
    using namespace N2Engine;
    using namespace N2Engine::Physics;
    using Math::Vector3;

    // Collects the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        [[nodiscard]] int Count(const std::string &text) const
        {
            return static_cast<int>(std::ranges::count_if(
                warnings, [&](const std::string &w) { return w.find(text) != std::string::npos; }));
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };

    float Distance(const Vector3 &a, const Vector3 &b)
    {
        const Vector3 d = a - b;
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }
}

// ===== The feature bit and a backend without joints (no physics backend needed in the build) =====

TEST(JointFeatureTest, TheJointsBitIsOptionalAndIsNotOneOfTheRequiredGroups)
{
    EXPECT_FALSE(PhysicsFeatureSet::None().Has(PhysicsFeature::Joints));
    EXPECT_FALSE(PhysicsFeatureSet::All().Has(PhysicsFeature::Joints)) << "All() is the required groups";
    const PhysicsFeatureSet withJoints = PhysicsFeatureSet::All().With(PhysicsFeature::Joints);
    EXPECT_TRUE(withJoints.Has(PhysicsFeature::Joints));
    EXPECT_TRUE(withJoints.IsComplete());
    EXPECT_TRUE(withJoints.Missing().empty());
    EXPECT_TRUE(PhysicsFeatureSet::All().IsComplete()) << "a backend without joints is still complete";
    EXPECT_FALSE(PhysicsFeatureSet::None().With(PhysicsFeature::Joints).IsComplete());
    EXPECT_EQ(PhysicsFeatureSet::None().Missing().size(), kAllPhysicsFeatures.size());
    EXPECT_EQ(ToString(PhysicsFeature::Joints), "joints");
}

TEST(JointFeatureTest, ABackendThatSaysNothingHasNoJointsAndCreatesNone)
{
    TestSupport::FakePhysicsBackend fake(false);
    EXPECT_FALSE(fake.GetImplementedFeatures().Has(PhysicsFeature::Joints));
    JointDesc desc;
    EXPECT_FALSE(fake.CreateJoint(desc).IsValid());
    fake.DestroyJoint(PhysicsJointHandle{}); // does nothing
    EXPECT_FALSE(fake.ConfigureJoint(PhysicsJointHandle{}, desc));
    EXPECT_FALSE(fake.IsJointValid(PhysicsJointHandle{}));
}

TEST(JointFeatureTest, AJointOnABackendWithoutJointsWarnsOnceAndDoesNothing)
{
    WarningCapture warnings;
    Application::GetInstance().Set3DPhysicsBackend(std::make_unique<TestSupport::FakePhysicsBackend>(false));

    auto object = GameObject::Create("Hinged");
    auto *joint = object->AddComponent<Joint>();
    joint->OnAttach();
    for (int i = 0; i < 5; ++i)
    {
        joint->OnFixedUpdate();
    }
    EXPECT_FALSE(joint->GetHandle().IsValid());
    EXPECT_EQ(warnings.Count("does not implement joints"), 1) << "once, not every step";
    EXPECT_EQ(warnings.Count("Hinged"), 1) << "the warning names the object";

    Application::GetInstance().Set3DPhysicsBackend(nullptr);
}

TEST(JointFeatureTest, AJointWithNoPhysicsBackendIsQuiet)
{
    WarningCapture warnings;
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    auto object = GameObject::Create("Loose");
    auto *joint = object->AddComponent<Joint>();
    joint->OnAttach();
    joint->OnFixedUpdate();
    EXPECT_FALSE(joint->GetHandle().IsValid());
    EXPECT_EQ(warnings.Count("joint"), 0);
}

TEST(JointComponentTest, ItIsRegisteredAndSavesItsFields)
{
    auto &registry = ComponentRegistry::Instance();
    ASSERT_TRUE(registry.IsRegistered("Joint"));

    auto object = GameObject::Create("Source");
    auto *joint = object->AddComponent<Joint>();
    joint->SetJointType(JointType::Hinge);
    joint->SetAnchor(Vector3(1.0f, 2.0f, 3.0f));
    joint->SetConnectedAnchor(Vector3(4.0f, 5.0f, 6.0f));
    joint->SetAxis(Vector3(0.0f, 0.0f, 1.0f));
    joint->SetCollideConnected(true);
    joint->SetLimits(true, -30.0f, 45.0f);

    const nlohmann::json saved = joint->Serialize();
    EXPECT_EQ(saved.at("_jointType"), "Hinge");

    auto other = GameObject::Create("Copy");
    auto *copy = other->AddComponent<Joint>();
    copy->Deserialize(saved);
    EXPECT_EQ(copy->GetJointType(), JointType::Hinge);
    EXPECT_NEAR(copy->GetAnchor().y, 2.0f, 1e-6f);
    EXPECT_NEAR(copy->GetConnectedAnchor().z, 6.0f, 1e-6f);
    EXPECT_NEAR(copy->GetAxis().z, 1.0f, 1e-6f);
    EXPECT_TRUE(copy->GetCollideConnected());
    EXPECT_TRUE(copy->GetUseLimits());
    EXPECT_NEAR(copy->GetLimitMin(), -30.0f, 1e-6f);
    EXPECT_NEAR(copy->GetLimitMax(), 45.0f, 1e-6f);
    EXPECT_EQ(copy->GetConnectedBody(), nullptr);
}


#ifdef N2_PHYSICS_TEST_BACKENDS

#ifdef N2ENGINE_SCRIPTING_ENABLED
#include "engine/scripting/LuaRuntime.hpp"
#endif

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;

        Vector3 PositionOf(const GameObject::Ptr &go) { return go->GetPositionable()->GetPosition(); }
    }

    template <typename Tag>
    class JointFeatureBackendTest : public ::testing::Test
    {
    };

    TYPED_TEST_SUITE(JointFeatureBackendTest, AllBackends, BackendNames);

    TYPED_TEST(JointFeatureBackendTest, ReportsJointsAndStaysCompleteForThePreset)
    {
        using Backend = typename TypeParam::Backend;
        EXPECT_TRUE(Backend::kImplementedFeatures.Has(PhysicsFeature::Joints));
        EXPECT_TRUE(Backend::kImplementedFeatures.IsComplete());
        EXPECT_TRUE(Backend::kImplementedFeatures.Missing().empty());
        Backend backend;
        EXPECT_TRUE(backend.GetImplementedFeatures().Has(PhysicsFeature::Joints));
    }

    // ===== The backend alone =====

    template <typename Tag>
    class JointBackendTest : public ::testing::Test
    {
    };

    TYPED_TEST_SUITE(JointBackendTest, AllBackends, BackendNames);

    TYPED_TEST(JointBackendTest, JointsAreMadeInSlotOrderAndEndWithEitherBody)
    {
        typename TypeParam::Backend backend;
        ASSERT_TRUE(backend.Initialize());
        const auto material = PhysicsMaterial::Default();
        const Math::Quaternion identity = Math::Quaternion::Identity;
        std::vector<PhysicsBodyHandle> bodies;
        for (int i = 0; i < 4; ++i)
        {
            const PhysicsBodyHandle body =
                backend.CreateDynamicBody(Vector3(static_cast<float>(i) * 3.0f, 5.0f, 0.0f), identity, 1.0f, nullptr, false);
            backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
            backend.SetGravityEnabled(body, false);
            bodies.push_back(body);
        }

        JointDesc desc;
        desc.type = JointType::Ball;
        std::vector<PhysicsJointHandle> joints;
        for (int i = 0; i < 3; ++i)
        {
            desc.bodyA = bodies[i];
            desc.bodyB = bodies[i + 1];
            desc.anchorA = Vector3(1.5f, 0.0f, 0.0f);
            desc.anchorB = Vector3(-1.5f, 0.0f, 0.0f);
            joints.push_back(backend.CreateJoint(desc));
            ASSERT_TRUE(joints.back().IsValid());
            EXPECT_EQ(joints.back().index, static_cast<std::uint32_t>(i)) << "slots in creation order";
        }
        for (const PhysicsJointHandle joint : joints)
        {
            EXPECT_TRUE(backend.IsJointValid(joint));
        }

        // The middle one ends; the next joint takes the lowest free slot, so the same sequence gives the same handles
        backend.DestroyJoint(joints[1]);
        EXPECT_FALSE(backend.IsJointValid(joints[1]));
        desc.bodyA = bodies[0];
        desc.bodyB = bodies[3];
        const PhysicsJointHandle reused = backend.CreateJoint(desc);
        EXPECT_EQ(reused.index, 1u);
        EXPECT_NE(reused.generation, joints[1].generation) << "the old handle to that slot stays stale";
        EXPECT_FALSE(backend.IsJointValid(joints[1]));
        EXPECT_TRUE(backend.IsJointValid(reused));

        // A body that is destroyed takes its joints with it
        backend.DestroyBody(bodies[0]);
        EXPECT_FALSE(backend.IsJointValid(joints[0]));
        EXPECT_FALSE(backend.IsJointValid(reused));
        EXPECT_TRUE(backend.IsJointValid(joints[2])) << "between bodies 2 and 3";
        backend.DestroyJoint(joints[0]); // a stale handle does nothing
    }

    TYPED_TEST(JointBackendTest, ADisabledBodySuspendsItsJointAndItResumes)
    {
        typename TypeParam::Backend backend;
        ASSERT_TRUE(backend.Initialize());
        const auto material = PhysicsMaterial::Default();
        const Math::Quaternion identity = Math::Quaternion::Identity;
        const PhysicsBodyHandle a = backend.CreateDynamicBody(Vector3(0, 5, 0), identity, 1.0f, nullptr, false);
        backend.AddBoxCollider(a, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
        JointDesc desc;
        desc.type = JointType::Fixed;
        desc.bodyA = a;
        desc.anchorB = Vector3(0, 5, 0);
        const PhysicsJointHandle joint = backend.CreateJoint(desc);
        ASSERT_TRUE(joint.IsValid());

        backend.SetBodyEnabled(a, false);
        backend.Update(kStep); // nothing in the simulation, nothing to fail on
        EXPECT_TRUE(backend.IsJointValid(joint));
        backend.SetBodyEnabled(a, true);
        for (int i = 0; i < 60; ++i)
        {
            backend.Update(kStep);
        }
        EXPECT_NEAR(backend.GetPosition(a).y, 5.0f, 0.02f) << "held to the world again";
    }

    TYPED_TEST(JointBackendTest, BadJointsAreRefusedWithAWarning)
    {
        typename TypeParam::Backend backend;
        ASSERT_TRUE(backend.Initialize());
        const Math::Quaternion identity = Math::Quaternion::Identity;
        const PhysicsBodyHandle a = backend.CreateDynamicBody(Vector3(0, 5, 0), identity, 1.0f, nullptr, false);
        const PhysicsBodyHandle b = backend.CreateDynamicBody(Vector3(3, 5, 0), identity, 1.0f, nullptr, false);

        WarningCapture warnings;
        JointDesc desc;
        desc.type = JointType::Hinge;
        EXPECT_FALSE(backend.CreateJoint(desc).IsValid()) << "no body A";
        desc.bodyA = a;
        desc.bodyB = a;
        EXPECT_FALSE(backend.CreateJoint(desc).IsValid()) << "a body to itself";
        desc.bodyB = PhysicsBodyHandle{7, 0};
        EXPECT_FALSE(backend.CreateJoint(desc).IsValid()) << "a stale body B";
        desc.bodyB = b;
        desc.axis = Vector3::Zero;
        EXPECT_FALSE(backend.CreateJoint(desc).IsValid()) << "a hinge with no axis";
        desc.axis = Vector3(0, 0, 1);
        desc.anchorA = Vector3(std::nanf(""), 0, 0);
        EXPECT_FALSE(backend.CreateJoint(desc).IsValid()) << "a non-finite anchor";
        EXPECT_EQ(warnings.warnings.size(), 5u) << "each refusal says so";

        desc.anchorA = Vector3::Zero;
        const PhysicsJointHandle joint = backend.CreateJoint(desc);
        ASSERT_TRUE(joint.IsValid());
        JointDesc other = desc;
        other.type = JointType::Slider;
        EXPECT_FALSE(backend.ConfigureJoint(joint, other)) << "a joint's type cannot change";
        EXPECT_TRUE(backend.ConfigureJoint(joint, desc));
        EXPECT_FALSE(backend.ConfigureJoint(PhysicsJointHandle{9, 0}, desc));
    }

    // ===== The component, on every backend =====

    template <typename Tag>
    class JointTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        using PhysicsCoreFixture<Tag>::Spawn;

        // A 1x1x1 dynamic box with gravity
        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const bool gravity = true)
        {
            const auto go = Spawn(name, position);
            go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            auto *body = go->AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(gravity);
            return go;
        }

        // A static 1x1x1 box
        GameObject::Ptr SpawnAnchor(const std::string &name, const Vector3 &position)
        {
            const auto go = Spawn(name, position);
            go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            return go;
        }
    };

    TYPED_TEST_SUITE(JointTest, AllBackends, BackendNames);

    TYPED_TEST(JointTest, AFixedJointHoldsABodyToAnotherAndToTheWorld)
    {
        const auto anchor = this->SpawnAnchor("Anchor", Vector3(0, 8, 0));
        const auto hanging = this->SpawnBox("Hanging", Vector3(0, 5, 0));
        auto *toBody = hanging->AddComponent<Joint>();
        toBody->SetJointType(JointType::Fixed);
        toBody->SetConnectedBody(anchor.get());
        toBody->SetConnectedAnchor(Vector3(0, -3, 0)); // the point of the anchor where the hanging box is

        const auto pinned = this->SpawnBox("Pinned", Vector3(10, 5, 0));
        auto *toWorld = pinned->AddComponent<Joint>();
        toWorld->SetJointType(JointType::Fixed);
        toWorld->SetConnectedAnchor(Vector3(10, 5, 0)); // no connected body: a world position

        const auto free = this->SpawnBox("Free", Vector3(20, 5, 0));

        this->Step(120);
        EXPECT_TRUE(toBody->GetHandle().IsValid());
        EXPECT_TRUE(toWorld->GetHandle().IsValid());
        ExpectVectorNear(PositionOf(hanging), Vector3(0, 5, 0), 0.02f);
        ExpectVectorNear(PositionOf(pinned), Vector3(10, 5, 0), 0.02f);
        EXPECT_LT(PositionOf(free).y, 0.0f) << "the same box with no joint falls";
    }

    TYPED_TEST(JointTest, AFixedJointWithAnOffCentreAnchorKeepsTheRelativePoseItWasMadeWith)
    {
        const auto anchor = this->SpawnAnchor("Anchor", Vector3(0, 8, 0));
        const auto arm = this->SpawnBox("Arm", Vector3(2, 8, 0));
        auto *joint = arm->AddComponent<Joint>();
        joint->SetConnectedBody(anchor.get());
        joint->SetAnchor(Vector3(-2, 0, 0)); // the anchor's origin, seen from the arm
        joint->SetConnectedAnchor(Vector3::Zero);

        // The arm hangs off the side of its weld: gravity would swing a loose arm down, a weld holds it and its rotation
        this->Step(180);
        ExpectVectorNear(PositionOf(arm), Vector3(2, 8, 0), 0.03f);
        const Math::Quaternion rotation = arm->GetPositionable()->GetRotation();
        EXPECT_NEAR(std::abs(rotation.GetW()), 1.0f, 0.01f) << "no rotation";
    }

    TYPED_TEST(JointTest, AHingeSwingsAboutItsAxisOnlyAndLimitsStopIt)
    {
        // Two arms from a pivot at (0,5,0) and (20,5,0), swinging down in the XY plane about Z
        const auto free = this->SpawnBox("FreeArm", Vector3(1, 5, 0));
        auto *freeHinge = free->AddComponent<Joint>();
        freeHinge->SetJointType(JointType::Hinge);
        freeHinge->SetAnchor(Vector3(-1, 0, 0));
        freeHinge->SetConnectedAnchor(Vector3(0, 5, 0));
        freeHinge->SetAxis(Vector3(0, 0, 1));

        const auto limited = this->SpawnBox("LimitedArm", Vector3(21, 5, 0));
        auto *limitedHinge = limited->AddComponent<Joint>();
        limitedHinge->SetJointType(JointType::Hinge);
        limitedHinge->SetAnchor(Vector3(-1, 0, 0));
        limitedHinge->SetConnectedAnchor(Vector3(20, 5, 0));
        limitedHinge->SetAxis(Vector3(0, 0, 1));

        this->Step();
        ASSERT_TRUE(limitedHinge->GetHandle().IsValid());
        limitedHinge->SetLimits(true, -30.0f, 30.0f); // live: the joint is not made again
        const PhysicsJointHandle handle = limitedHinge->GetHandle();

        float freeLowest = 5.0f;
        for (int i = 0; i < 180; ++i)
        {
            this->Step();
            const Vector3 p = PositionOf(free);
            EXPECT_NEAR(Distance(p, Vector3(0, 5, 0)), 1.0f, 0.03f) << "step " << i << ": the pivot holds";
            EXPECT_NEAR(p.z, 0.0f, 0.01f) << "step " << i << ": no movement along the axis";
            freeLowest = std::min(freeLowest, p.y);
        }
        EXPECT_EQ(limitedHinge->GetHandle(), handle);
        EXPECT_LT(freeLowest, 4.2f) << "a free hinge swings through the bottom";

        // 30 degrees down from horizontal is y = 5 - sin(30) = 4.5 at the end of a 1 m arm
        const Vector3 p = PositionOf(limited);
        EXPECT_GT(p.y, 4.4f) << "stopped at the limit";
        EXPECT_LT(p.y, 4.6f);
        EXPECT_NEAR(Distance(p, Vector3(20, 5, 0)), 1.0f, 0.03f);
    }

    TYPED_TEST(JointTest, ABallJointKeepsItsPointButRotatesAndSwingsFreely)
    {
        const auto ball = this->SpawnBox("Ball", Vector3(1, 5, 0));
        auto *joint = ball->AddComponent<Joint>();
        joint->SetJointType(JointType::Ball);
        joint->SetAnchor(Vector3(-1, 0, 0));
        joint->SetConnectedAnchor(Vector3(0, 5, 0));

        this->Step();
        ASSERT_TRUE(joint->GetHandle().IsValid());
        ball->GetComponent<Rigidbody>()->SetVelocity(Vector3(0, 0, 3)); // out of the swing plane

        float widest = 0.0f;
        for (int i = 0; i < 120; ++i)
        {
            this->Step();
            const Vector3 p = PositionOf(ball);
            EXPECT_NEAR(Distance(p, Vector3(0, 5, 0)), 1.0f, 0.05f) << "step " << i << ": the point holds";
            widest = std::max(widest, std::abs(p.z));
        }
        EXPECT_GT(widest, 0.2f) << "free in every direction, so it left the plane";
    }

    TYPED_TEST(JointTest, ASliderMovesAlongItsAxisOnlyAndLimitsStopIt)
    {
        const auto free = this->SpawnBox("FreeSlider", Vector3(0, 5, 0));
        auto *freeSlider = free->AddComponent<Joint>();
        freeSlider->SetJointType(JointType::Slider);
        freeSlider->SetConnectedAnchor(Vector3(0, 5, 0));
        freeSlider->SetAxis(Vector3(1, 0, 0));

        const auto limited = this->SpawnBox("LimitedSlider", Vector3(20, 5, 0));
        auto *limitedSlider = limited->AddComponent<Joint>();
        limitedSlider->SetJointType(JointType::Slider);
        limitedSlider->SetConnectedAnchor(Vector3(20, 5, 0));
        limitedSlider->SetAxis(Vector3(1, 0, 0));
        limitedSlider->SetLimits(true, -1.0f, 1.0f);

        this->Step();
        ASSERT_TRUE(freeSlider->GetHandle().IsValid());
        free->GetComponent<Rigidbody>()->SetVelocity(Vector3(3, 0, 0));
        limited->GetComponent<Rigidbody>()->SetVelocity(Vector3(3, 0, 0));
        this->Step(60);

        const Vector3 f = PositionOf(free);
        EXPECT_GT(f.x, 2.0f) << "slides along the axis";
        EXPECT_NEAR(f.y, 5.0f, 0.02f) << "gravity does not move it off the axis";
        EXPECT_NEAR(f.z, 0.0f, 0.02f);

        const Vector3 l = PositionOf(limited);
        EXPECT_GT(l.x, 20.0f + 0.9f);
        EXPECT_LT(l.x, 20.0f + 1.06f) << "stopped at the limit";
        EXPECT_NEAR(l.y, 5.0f, 0.02f);
    }

    TYPED_TEST(JointTest, ADistanceJointIsARodWithoutLimitsAndARopeWithThem)
    {
        const auto anchor = this->SpawnAnchor("Anchor", Vector3(0, 8, 0));
        const auto rod = this->SpawnBox("Rod", Vector3(3, 8, 0));
        auto *rodJoint = rod->AddComponent<Joint>();
        rodJoint->SetJointType(JointType::Distance);
        rodJoint->SetConnectedBody(anchor.get());

        const auto ropeAnchor = this->SpawnAnchor("RopeAnchor", Vector3(20, 8, 0));
        const auto rope = this->SpawnBox("Rope", Vector3(20, 7, 0));
        auto *ropeJoint = rope->AddComponent<Joint>();
        ropeJoint->SetJointType(JointType::Distance);
        ropeJoint->SetConnectedBody(ropeAnchor.get());
        ropeJoint->SetLimits(true, 0.0f, 2.0f);

        float rodLowest = 8.0f;
        this->Step();
        ASSERT_TRUE(rodJoint->GetHandle().IsValid());
        ASSERT_TRUE(ropeJoint->GetHandle().IsValid());
        for (int i = 0; i < 150; ++i)
        {
            this->Step();
            const Vector3 p = PositionOf(rod);
            EXPECT_NEAR(Distance(p, Vector3(0, 8, 0)), 3.0f, 0.06f) << "step " << i << ": a rod keeps its length";
            rodLowest = std::min(rodLowest, p.y);
        }
        EXPECT_LT(rodLowest, 6.0f) << "and swings down on it";

        const Vector3 r = PositionOf(rope);
        EXPECT_NEAR(Distance(r, Vector3(20, 8, 0)), 2.0f, 0.1f) << "a rope hangs at its longest";
        EXPECT_NEAR(r.y, 6.0f, 0.15f);
    }

    TYPED_TEST(JointTest, JoinedBodiesCollideOnlyWhenCollideConnectedIsOn)
    {
        // Two boxes overlapping by half their width, weightless: free bodies push each other apart, joined ones do not
        const auto joinedA = this->SpawnBox("JoinedA", Vector3(0, 5, 0), false);
        const auto joinedB = this->SpawnBox("JoinedB", Vector3(0.5f, 5, 0), false);
        auto *joint = joinedB->AddComponent<Joint>();
        joint->SetJointType(JointType::Fixed);
        joint->SetConnectedBody(joinedA.get());
        joint->SetConnectedAnchor(Vector3(0.5f, 0, 0));
        ASSERT_FALSE(joint->GetCollideConnected()) << "off by default";

        const auto looseA = this->SpawnBox("LooseA", Vector3(20, 5, 0), false);
        const auto looseB = this->SpawnBox("LooseB", Vector3(20.5f, 5, 0), false);

        this->Step(60);
        EXPECT_NEAR(Distance(PositionOf(joinedA), PositionOf(joinedB)), 0.5f, 0.02f) << "no push between joined bodies";
        EXPECT_GT(Distance(PositionOf(looseA), PositionOf(looseB)), 0.8f) << "the control pair was pushed apart";
    }

    TYPED_TEST(JointTest, RemovingTheComponentEndsTheJoint)
    {
        const auto pinned = this->SpawnBox("Pinned", Vector3(0, 5, 0));
        auto *joint = pinned->AddComponent<Joint>();
        joint->SetConnectedAnchor(Vector3(0, 5, 0));
        this->Step(30);
        ASSERT_TRUE(joint->GetHandle().IsValid());
        const PhysicsJointHandle handle = joint->GetHandle();
        EXPECT_TRUE(this->BackendRef().IsJointValid(handle));
        EXPECT_NEAR(PositionOf(pinned).y, 5.0f, 0.02f);

        pinned->RemoveComponent<Joint>();
        this->Step(30);
        EXPECT_FALSE(this->BackendRef().IsJointValid(handle));
        EXPECT_LT(PositionOf(pinned).y, 4.5f) << "free to fall";
    }

    TYPED_TEST(JointTest, ADeactivatedObjectLosesItsJointAndGetsItBackWhenActivated)
    {
        const auto pinned = this->SpawnBox("Pinned", Vector3(0, 5, 0));
        auto *joint = pinned->AddComponent<Joint>();
        joint->SetConnectedAnchor(Vector3(0, 5, 0));
        this->Step(10);
        ASSERT_TRUE(joint->GetHandle().IsValid());

        pinned->SetActive(false);
        this->Step(10);
        EXPECT_FALSE(joint->GetHandle().IsValid());
        pinned->SetActive(true);
        this->Step(60);
        EXPECT_TRUE(joint->GetHandle().IsValid());
        EXPECT_NEAR(PositionOf(pinned).y, 5.0f, 0.03f);
    }

    TYPED_TEST(JointTest, AJointWaitsForABodyThatIsNotThereYetAndFollowsAReplacedOne)
    {
        // The connected object has no body when the joint's first step runs: the joint waits (it does not fall back to
        // the world), and is made once the body exists
        const auto anchor = this->Spawn("LateAnchor", Vector3(0, 8, 0));
        const auto hanging = this->SpawnBox("Hanging", Vector3(0, 5, 0));
        auto *joint = hanging->AddComponent<Joint>();
        joint->SetConnectedBody(anchor.get());
        joint->SetConnectedAnchor(Vector3(0, -3, 0));
        this->Step(5);
        EXPECT_FALSE(joint->GetHandle().IsValid());

        anchor->AddComponent<BoxCollider>()->SetSize(Vector3(1, 1, 1));
        this->Step(60);
        ASSERT_TRUE(joint->GetHandle().IsValid());
        EXPECT_NEAR(PositionOf(hanging).y, 5.0f, 0.2f) << "caught by the joint once it exists";

        // Replacing the body (a changed body type) makes the joint again on the new one
        const PhysicsJointHandle before = joint->GetHandle();
        hanging->GetComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        this->Step(5);
        hanging->GetComponent<Rigidbody>()->SetBodyType(BodyType::Dynamic);
        this->Step(5);
        EXPECT_TRUE(joint->GetHandle().IsValid());
        EXPECT_TRUE(this->BackendRef().IsJointValid(joint->GetHandle()));
        (void)before;
    }

#ifdef N2ENGINE_SCRIPTING_ENABLED
    // ===== Lua =====

#ifdef N2ENGINE_JOLT_ENABLED
    using LuaJointBackendTag = JoltTag;
#else
    using LuaJointBackendTag = PhysXTag;
#endif

    class LuaJointTest : public JointTest<LuaJointBackendTag>
    {
    protected:
        static void SetUpTestSuite()
        {
            JointTest<LuaJointBackendTag>::SetUpTestSuite();
            ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
        }

        static sol::state &Lua() { return Scripting::LuaRuntime::Instance().GetState(); }

        static void Run(const std::string &code)
        {
            const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error err = result;
                ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << code;
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
            return result.template get<T>();
        }

        static bool Raises(const std::string &code)
        {
            return !Lua().safe_script(code, sol::script_pass_on_error).valid();
        }
    };

    TEST_F(LuaJointTest, AScriptAddsAJointConfiguresItAndItConstrains)
    {
        Run(R"(
            ceiling = GameObject.Create("Ceiling")
            ceiling:CreatePositionable()
            ceiling:GetPositionable():SetPosition(Vector3(0, 8, 0))
            ceiling:AddComponent("BoxCollider")
            SceneManager.GetCurrentScene():AddRootGameObject(ceiling)

            bob = GameObject.Create("Bob")
            bob:CreatePositionable()
            bob:GetPositionable():SetPosition(Vector3(0, 5, 0))
            bob:AddComponent("BoxCollider")
            bob:AddComponent("Rigidbody")
            SceneManager.GetCurrentScene():AddRootGameObject(bob)

            joint = bob:AddComponent("Joint")
            joint:SetJointType(JointType.Fixed)
            joint:SetConnectedBody(ceiling)
            joint:SetConnectedAnchor(Vector3(0, -3, 0))
            joint:SetCollideConnected(false)
            joint:SetLimits(false, -10, 10)
        )");
        EXPECT_TRUE(Eval<bool>("joint:IsValid()"));
        EXPECT_EQ(Eval<int>("joint:GetJointType()"), static_cast<int>(JointType::Fixed));
        EXPECT_TRUE(Eval<bool>("joint:GetConnectedBody() == ceiling"));
        EXPECT_NEAR(Eval<float>("joint:GetConnectedAnchor().y"), -3.0f, 1e-6f);
        EXPECT_FALSE(Eval<bool>("joint:GetCollideConnected()"));
        EXPECT_FALSE(Eval<bool>("joint:GetUseLimits()"));
        EXPECT_NEAR(Eval<float>("joint:GetLimitMin()"), -10.0f, 1e-6f);

        Step(90);
        EXPECT_TRUE(Eval<bool>("joint:IsCreated()"));
        EXPECT_NEAR(Eval<float>("bob:GetPositionable():GetPosition().y"), 5.0f, 0.03f) << "held by the joint";

        Run("joint:SetConnectedBody(nil)");
        EXPECT_TRUE(Eval<bool>("joint:GetConnectedBody() == nil"));
    }

    TEST_F(LuaJointTest, BadArgumentsAreLuaErrors)
    {
        Run(R"(
            thing = GameObject.Create("Thing")
            joint = thing:AddComponent("Joint")
            SceneManager.GetCurrentScene():AddRootGameObject(thing)
        )");
        for (const char *bad : {"joint:SetAnchor(1)", "joint:SetAnchor(nil)", "joint:SetAxis('x')",
                                "joint:SetConnectedAnchor(Vector3(0/0, 0, 0))", "joint:SetConnectedBody(5)",
                                "joint:SetLimits(1, 0, 1)", "joint:SetLimits(true, 'a', 1)",
                                "joint:SetLimits(true, 0, math.huge)"})
        {
            EXPECT_TRUE(Raises(bad)) << bad;
        }
        EXPECT_EQ(Eval<int>("joint:GetJointType()"), static_cast<int>(JointType::Fixed)) << "nothing changed";
    }
#endif // N2ENGINE_SCRIPTING_ENABLED
}

#endif // a physics backend is in this build
