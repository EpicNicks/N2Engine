// 2D joints (issue #445): the Joint2D component on Box2D's weld, revolute, prismatic, wheel, distance and motor joints, its
// lifecycle, breaking, serialization, and rollback (the joints in Physics2DWorld::SaveState, claimed back by their components).
// docs/physics2d.html#joints. With N2ENGINE_USE_BOX2D=OFF only the stripped test is compiled.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Joint2D.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;
using namespace N2Engine::Physics2D;
using Math::Vector2;
using Math::Vector3;

namespace
{
    class Joint2DTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        void SetUp() override
        {
            Layers::ResetToDefaults();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
            SceneManager::AddScene(Scene::Create(std::string("Joint2D_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
        }

        void TearDown() override
        {
            SceneManager::AddScene(Scene::Create("Joint2D_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
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

        /// A dynamic circle (or box) body at (x, y)
        GameObject::Ptr Body(const std::string &name, const float x, const float y, const bool box = false)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Vector3(x, y, 0.0f));
            _scene->AddRootGameObject(go);
            go->AddComponent<Rigidbody2D>();
            if (box)
            {
                go->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.2f, 0.2f));
            }
            else
            {
                go->AddComponent<CircleCollider2D>()->SetRadius(0.2f);
            }
            return go;
        }

        static Vector2 At(const GameObject::Ptr &go)
        {
            const Vector3 p = go->GetPositionable()->GetPosition();
            return Vector2(p.x, p.y);
        }

        static float Distance(const Vector2 &a, const Vector2 &b) { return (a - b).Length(); }

        /// A joint on `go` to the world point `pivot`, its anchor on the body at the pivot's offset
        static Joint2D *ToWorld(const GameObject::Ptr &go, const JointType2D type, const Vector2 &pivot)
        {
            auto *joint = go->AddComponent<Joint2D>();
            joint->SetJointType(type);
            joint->SetAnchor(pivot - At(go));
            joint->SetConnectedAnchor(pivot);
            return joint;
        }
    };

    class BreakListener final : public Component
    {
    public:
        explicit BreakListener(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "BreakListener"; }
        void OnJointBreak2D(Joint2D &joint) override
        {
            ++breaks;
            wasCreatedWhenCalled = joint.IsCreated();
        }
        int breaks = 0;
        bool wasCreatedWhenCalled = true;
    };
}

#ifdef N2ENGINE_BOX2D_ENABLED

TEST_F(Joint2DTest, AHingeToTheWorldSwingsAroundItsPivot)
{
    const auto bob = Body("Bob", 2.0f, 0.0f);
    Joint2D *joint = ToWorld(bob, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    EXPECT_FALSE(joint->IsCreated()) << "made in the first fixed step";
    Step(40);
    ASSERT_TRUE(joint->IsCreated());
    EXPECT_NEAR(Distance(At(bob), Vector2(0.0f, 0.0f)), 2.0f, 0.05f);
    EXPECT_LT(At(bob).y, -0.5f) << "it swung down";
    EXPECT_LT(joint->GetJointAngle(), -10.0f) << "degrees from the pose it was made at (clockwise is negative)";
    EXPECT_GT(joint->GetReactionForce().Length(), 0.0f);
}

TEST_F(Joint2DTest, AHingeMotorDrivesItsSpeedAndLimitsStopIt)
{
    const auto wheel = Body("Wheel", 0.0f, 0.0f);
    wheel->GetComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    Joint2D *joint = ToWorld(wheel, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    // A modest torque: the wheel needs about 0.01 to reach 90 degrees a second, and a limit holds it against that. Box2D solves a limit
    // as a soft constraint after the motor, so a torque of 1000 on this 0.0025 inertia pushes straight through it.
    joint->SetMotor(true, 90.0f, 0.05f);
    Step(20);
    EXPECT_NEAR(wheel->GetComponent<Rigidbody2D>()->GetAngularVelocity(), 3.14159265f / 2.0f, 0.05f) << "90 degrees a second";

    joint->SetLimits(true, -10.0f, 30.0f);
    EXPECT_TRUE(joint->GetUseLimits());
    Step(120);
    EXPECT_LE(joint->GetJointAngle(), 31.0f);
    EXPECT_GE(joint->GetJointAngle(), 25.0f) << "the motor holds it against the upper limit";

    // Limits that do not hold the zero pose are refused, and the old ones stay
    joint->SetLimits(true, 10.0f, 20.0f);
    EXPECT_FLOAT_EQ(joint->GetLimitMin(), -10.0f);
    EXPECT_FALSE(Joint2D::DescribeProblem(JointType2D::Hinge, true, -179.0f, 0.0f, false, 0.0f, 0.0f).empty());
    EXPECT_TRUE(Joint2D::DescribeProblem(JointType2D::Distance, true, 1.0f, 2.0f, false, 0.0f, 0.0f).empty());
}

TEST_F(Joint2DTest, ASliderMovesOnlyAlongItsAxisWithinItsLimits)
{
    const auto block = Body("Block", 0.0f, 0.0f, true);
    Joint2D *joint = ToWorld(block, JointType2D::Slider, Vector2(0.0f, 0.0f));
    joint->SetAxis(Vector2(0.0f, 1.0f));
    block->GetComponent<Rigidbody2D>()->SetLinearVelocity(Vector2(3.0f, 0.0f));
    Step(30);
    EXPECT_NEAR(At(block).x, 0.0f, 0.02f) << "the sideways velocity is taken by the joint";
    EXPECT_LT(At(block).y, -0.5f) << "it falls along the axis";
    EXPECT_NEAR(joint->GetJointTranslation(), At(block).y, 0.02f);

    joint->SetLimits(true, -1.5f, 0.0f);
    Step(120);
    EXPECT_GT(At(block).y, -1.55f);
}

TEST_F(Joint2DTest, ADistanceJointIsARodARopeOrASpring)
{
    // Rod: the length it was made at, whatever it does
    const auto rod = Body("Rod", 0.0f, -2.0f);
    rod->GetComponent<Rigidbody2D>()->SetLinearVelocity(Vector2(4.0f, 0.0f));
    Joint2D *rodJoint = ToWorld(rod, JointType2D::Distance, Vector2(0.0f, 0.0f));
    rodJoint->SetAnchor(Vector2(0.0f, 0.0f));

    // Rope: limits [0, 2] let it come closer, never further
    const auto rope = Body("Rope", 10.0f, -2.0f);
    rope->GetComponent<Rigidbody2D>()->SetLinearVelocity(Vector2(0.0f, 8.0f));
    Joint2D *ropeJoint = ToWorld(rope, JointType2D::Distance, Vector2(10.0f, 0.0f));
    ropeJoint->SetAnchor(Vector2(0.0f, 0.0f));
    ropeJoint->SetLimits(true, 0.0f, 2.0f);

    // Spring: gravity stretches it past its length
    const auto spring = Body("Spring", 20.0f, -2.0f);
    Joint2D *springJoint = ToWorld(spring, JointType2D::Distance, Vector2(20.0f, 0.0f));
    springJoint->SetAnchor(Vector2(0.0f, 0.0f));
    springJoint->SetSpring(true, 1.0f, 0.1f);

    float ropeShortest = 10.0f, springLongest = 0.0f;
    for (int i = 0; i < 60; ++i)
    {
        Step();
        EXPECT_NEAR(Distance(At(rod), Vector2(0.0f, 0.0f)), 2.0f, 0.05f) << i;
        EXPECT_LT(Distance(At(rope), Vector2(10.0f, 0.0f)), 2.05f) << i;
        ropeShortest = std::min(ropeShortest, Distance(At(rope), Vector2(10.0f, 0.0f)));
        springLongest = std::max(springLongest, Distance(At(spring), Vector2(20.0f, 0.0f)));
    }
    EXPECT_LT(ropeShortest, 1.5f) << "a rope goes slack";
    EXPECT_GT(springLongest, 2.1f) << "a spring stretches";
    EXPECT_NEAR(rodJoint->GetJointTranslation(), 2.0f, 0.05f) << "a distance joint reads its length";
}

TEST_F(Joint2DTest, AFixedJointWeldsTwoBodiesAndAWheelSpinsOnItsAxle)
{
    const auto a = Body("A", 0.0f, 0.0f, true);
    const auto b = Body("B", 1.0f, 0.0f, true);
    auto *weld = b->AddComponent<Joint2D>();
    weld->SetConnectedBody(a.get());
    weld->SetAnchor(Vector2(-0.5f, 0.0f));
    weld->SetConnectedAnchor(Vector2(0.5f, 0.0f));
    a->GetComponent<Rigidbody2D>()->SetAngularVelocity(1.0f);

    const auto car = Body("Car", 10.0f, 0.0f, true);
    car->GetComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    const auto tyre = Body("Tyre", 10.0f, -0.5f);
    tyre->GetComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    auto *wheel = tyre->AddComponent<Joint2D>();
    wheel->SetJointType(JointType2D::Wheel);
    wheel->SetConnectedBody(car.get());
    wheel->SetConnectedAnchor(Vector2(0.0f, -0.5f));
    wheel->SetAxis(Vector2(0.0f, 1.0f));
    wheel->SetMotor(true, -180.0f, 100.0f);

    Step(60);
    ASSERT_TRUE(weld->IsCreated());
    EXPECT_NEAR(Distance(At(a), At(b)), 1.0f, 0.03f) << "welded at the distance they were made at";
    EXPECT_GT(At(a).y, -100.0f);
    EXPECT_LT(At(a).y, -1.0f) << "they fall together";
    ASSERT_TRUE(wheel->IsCreated());
    EXPECT_NEAR(tyre->GetComponent<Rigidbody2D>()->GetAngularVelocity() - car->GetComponent<Rigidbody2D>()->GetAngularVelocity(),
                -3.14159265f, 0.1f) << "-180 degrees a second, relative to the car";
}

TEST_F(Joint2DTest, ATargetJointDragsTheAnchorToTheTarget)
{
    const auto puck = Body("Puck", 0.0f, 0.0f);
    puck->GetComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    auto *joint = puck->AddComponent<Joint2D>();
    joint->SetJointType(JointType2D::Target);
    joint->SetTarget(Vector2(3.0f, 2.0f));
    joint->SetSpring(false, 5.0f, 1.0f); // Target always uses its spring
    Step(150);
    ASSERT_TRUE(joint->IsCreated());
    EXPECT_NEAR(At(puck).x, 3.0f, 0.1f);
    EXPECT_NEAR(At(puck).y, 2.0f, 0.1f);

    // Moving the target keeps the joint
    const JointHandle2D handle = joint->GetHandle();
    joint->SetTarget(Vector2(-2.0f, 0.0f));
    Step(150);
    EXPECT_EQ(joint->GetHandle(), handle);
    EXPECT_NEAR(At(puck).x, -2.0f, 0.1f);
    EXPECT_NEAR(At(puck).y, 0.0f, 0.1f);
}

TEST_F(Joint2DTest, AJointPastItsBreakForceBreaksTellsTheObjectAndDisablesItself)
{
    const auto bob = Body("Bob", 2.0f, 0.0f);
    auto *listener = bob->AddComponent<BreakListener>();
    Joint2D *joint = ToWorld(bob, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    joint->SetBreakForce(1000.0f);
    Step(20);
    ASSERT_TRUE(joint->IsCreated()) << "a light pendulum does not reach 1000 N";
    EXPECT_EQ(listener->breaks, 0);

    joint->SetBreakForce(0.01f);
    Step(2);
    EXPECT_EQ(listener->breaks, 1);
    EXPECT_FALSE(listener->wasCreatedWhenCalled) << "already ended when the object is told";
    EXPECT_FALSE(joint->IsCreated());
    EXPECT_FALSE(joint->IsActiveSelf());
    const Vector2 freed = At(bob);
    Step(20);
    EXPECT_LT(At(bob).y, freed.y - 0.3f) << "it falls freely";
    EXPECT_GT(Distance(At(bob), Vector2(0.0f, 0.0f)), 2.1f);

    // Enabling it again makes a new joint, from the pose then
    joint->SetBreakForce(0.0f);
    joint->SetActive(true);
    Step();
    EXPECT_TRUE(joint->IsCreated());
    EXPECT_EQ(listener->breaks, 1);
}

TEST_F(Joint2DTest, ItWaitsForTheConnectedBodyAndEndsWithEitherBodyOrTheComponent)
{
    const auto a = Body("A", 0.0f, 0.0f);
    const auto other = GameObject::Create("Later");
    other->CreatePositionable();
    other->GetPositionable()->SetPosition(Vector3(1.0f, 0.0f, 0.0f));
    _scene->AddRootGameObject(other);
    auto *joint = a->AddComponent<Joint2D>();
    joint->SetConnectedBody(other.get());
    Step(2);
    EXPECT_FALSE(joint->IsCreated()) << "the connected object has no body yet: waited for, not the world";

    other->AddComponent<Rigidbody2D>();
    other->AddComponent<CircleCollider2D>();
    Step();
    ASSERT_TRUE(joint->IsCreated());

    // The connected body removed: the joint goes with it (Box2D ends a body's joints)
    other->GetComponent<Rigidbody2D>()->SetGravityScale(0.0f);
    auto *rigidbody = other->GetComponent<Rigidbody2D>();
    other->RemoveComponent(rigidbody);
    Step();
    EXPECT_TRUE(joint->IsCreated()) << "made again to the collider's own static body";

    a->RemoveComponent(joint);
    Step();
    EXPECT_NE(_scene->GetPhysics2DWorld(), nullptr);
}

TEST_F(Joint2DTest, ItsSettingsAndConnectedBodySurviveASaveAndLoad)
{
    const auto a = Body("A", 0.0f, 0.0f);
    const auto b = Body("B", 1.0f, 0.0f);
    auto *joint = b->AddComponent<Joint2D>();
    joint->SetJointType(JointType2D::Slider);
    joint->SetConnectedBody(a.get());
    joint->SetAnchor(Vector2(0.25f, 0.5f));
    joint->SetConnectedAnchor(Vector2(-0.5f, 0.0f));
    joint->SetAxis(Vector2(0.0f, 1.0f));
    joint->SetCollideConnected(true);
    joint->SetLimits(true, -1.0f, 2.0f);
    joint->SetSpring(true, 3.0f, 0.4f);
    joint->SetMotor(true, 1.5f, 20.0f);
    joint->SetMaxForce(55.0f);
    joint->SetBreakForce(300.0f);
    joint->SetBreakTorque(40.0f);

    const auto loaded = Scene::FromJSON(_scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto back = loaded->FindGameObject("B");
    ASSERT_NE(back, nullptr);
    const auto *joined = back->GetComponent<Joint2D>();
    ASSERT_NE(joined, nullptr);
    EXPECT_EQ(joined->GetJointType(), JointType2D::Slider);
    EXPECT_EQ(joined->GetConnectedBody(), loaded->FindGameObject("A").get()) << "the reference is to the loaded A";
    EXPECT_FLOAT_EQ(joined->GetAnchor().y, 0.5f);
    EXPECT_FLOAT_EQ(joined->GetConnectedAnchor().x, -0.5f);
    EXPECT_FLOAT_EQ(joined->GetAxis().y, 1.0f);
    EXPECT_TRUE(joined->GetCollideConnected());
    EXPECT_TRUE(joined->GetUseLimits());
    EXPECT_FLOAT_EQ(joined->GetLimitMax(), 2.0f);
    EXPECT_TRUE(joined->GetUseSpring());
    EXPECT_FLOAT_EQ(joined->GetSpringFrequency(), 3.0f);
    EXPECT_FLOAT_EQ(joined->GetSpringDamping(), 0.4f);
    EXPECT_TRUE(joined->GetUseMotor());
    EXPECT_FLOAT_EQ(joined->GetMotorSpeed(), 1.5f);
    EXPECT_FLOAT_EQ(joined->GetMaxMotorForce(), 20.0f);
    EXPECT_FLOAT_EQ(joined->GetMaxForce(), 55.0f);
    EXPECT_FLOAT_EQ(joined->GetBreakForce(), 300.0f);
    EXPECT_FLOAT_EQ(joined->GetBreakTorque(), 40.0f);
}

TEST_F(Joint2DTest, TheEditorRefusesSettingsTheJointCannotUse)
{
    const auto a = Body("A", 0.0f, 0.0f);
    auto *joint = a->AddComponent<Joint2D>();
    joint->SetJointType(JointType2D::Hinge);
    EXPECT_THROW(joint->SetEditorFields({{"_useLimits", true}, {"_limitMin", 5.0f}, {"_limitMax", 10.0f}}, nullptr), std::invalid_argument);
    EXPECT_FALSE(joint->GetUseLimits()) << "nothing was applied";
    EXPECT_THROW(joint->SetEditorFields({{"_useSpring", true}, {"_springFrequency", -1.0f}}, nullptr), std::invalid_argument);
    EXPECT_THROW(joint->SetEditorFields({{"_maxMotorForce", -1.0f}}, nullptr), std::invalid_argument);
    EXPECT_NO_THROW(joint->SetEditorFields({{"_useLimits", true}, {"_limitMin", -45.0f}, {"_limitMax", 45.0f}}, nullptr));
    EXPECT_TRUE(joint->GetUseLimits());
    EXPECT_FLOAT_EQ(joint->GetLimitMin(), -45.0f);
}

// ===== Rollback: the joints are in the 2D state, and their components take them back =====

namespace
{
    /// A chain of three links hung from the world, a welded pair, a wheel and a sliding block over a floor: every joint type
    void BuildRig(Scene &scene, std::vector<GameObject::Ptr> &bodies)
    {
        const auto make = [&scene, &bodies](const std::string &name, const float x, const float y)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Vector3(x, y, 0.0f));
            scene.AddRootGameObject(go);
            go->AddComponent<Rigidbody2D>();
            go->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.2f, 0.1f));
            bodies.push_back(go);
            return go;
        };
        auto floor = GameObject::Create("Floor");
        floor->CreatePositionable();
        floor->GetPositionable()->SetPosition(Vector3(0.0f, -6.0f, 0.0f));
        scene.AddRootGameObject(floor);
        floor->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(30.0f, 1.0f));

        GameObject::Ptr previous;
        for (int i = 0; i < 3; ++i)
        {
            auto link = make("Link" + std::to_string(i), 0.5f + static_cast<float>(i), 0.0f);
            auto *joint = link->AddComponent<Joint2D>();
            joint->SetJointType(JointType2D::Hinge);
            joint->SetAnchor(Vector2(-0.5f, 0.0f));
            if (previous)
            {
                joint->SetConnectedBody(previous.get());
                joint->SetConnectedAnchor(Vector2(0.5f, 0.0f));
            }
            previous = link;
        }
        auto a = make("WeldA", 6.0f, 0.0f);
        auto b = make("WeldB", 6.5f, 0.0f);
        auto *weld = b->AddComponent<Joint2D>();
        weld->SetConnectedBody(a.get());
        weld->SetSpring(true, 4.0f, 0.3f);
        auto rope = make("Rope", 9.0f, -1.0f);
        auto *distance = rope->AddComponent<Joint2D>();
        distance->SetJointType(JointType2D::Distance);
        distance->SetConnectedAnchor(Vector2(9.0f, 1.0f));
        distance->SetSpring(true, 2.0f, 0.2f);
        auto car = make("Car", 12.0f, -3.0f);
        auto tyre = make("Tyre", 12.0f, -3.6f);
        auto *wheel = tyre->AddComponent<Joint2D>();
        wheel->SetJointType(JointType2D::Wheel);
        wheel->SetConnectedBody(car.get());
        wheel->SetAxis(Vector2(0.0f, 1.0f));
        wheel->SetMotor(true, 120.0f, 50.0f);
        auto block = make("Block", 15.0f, 0.0f);
        auto *slider = block->AddComponent<Joint2D>();
        slider->SetJointType(JointType2D::Slider);
        slider->SetConnectedAnchor(Vector2(15.0f, 0.0f));
        slider->SetAxis(Vector2(0.6f, -0.8f));
        auto puck = make("Puck", 18.0f, 0.0f);
        auto *target = puck->AddComponent<Joint2D>();
        target->SetJointType(JointType2D::Target);
        target->SetTarget(Vector2(19.0f, 1.0f));
    }

    std::vector<Vector3> Poses(const std::vector<GameObject::Ptr> &bodies)
    {
        std::vector<Vector3> out;
        for (const auto &body : bodies)
        {
            out.push_back(body->GetPositionable()->GetPosition());
        }
        return out;
    }
}

TEST_F(Joint2DTest, ARestoredWorldWithJointsStepsExactlyLikeTheWorldThatWasSaved)
{
    std::vector<GameObject::Ptr> bodies;
    BuildRig(*_scene, bodies);
    Step(30);
    Physics2DWorld *const world = _scene->GetPhysics2DWorld();
    ASSERT_NE(world, nullptr);
    std::vector<JointHandle2D> handles;
    for (const auto &body : bodies)
    {
        if (const auto *joint = body->GetComponent<Joint2D>())
        {
            ASSERT_TRUE(joint->IsCreated()) << body->GetName();
            handles.push_back(joint->GetHandle());
        }
    }
    ASSERT_EQ(handles.size(), 8u);

    std::vector<std::byte> saved;
    ASSERT_TRUE(world->SaveState(saved));
    const std::uint64_t deepAtSave = world->HashState();
    Step(60);
    const std::uint64_t deepAfter = world->HashState();
    const std::uint64_t simulatedAfter = world->HashSimulatedState();
    const std::vector<Vector3> posesAfter = Poses(bodies);
    ASSERT_NE(deepAfter, deepAtSave);

    for (int round = 0; round < 2; ++round)
    {
        ASSERT_TRUE(world->RestoreState(saved));
        EXPECT_EQ(world->HashState(), deepAtSave) << "Box2D's state, joints included";
        Step(60);
        EXPECT_EQ(world->HashState(), deepAfter) << "round " << round;
        EXPECT_EQ(world->HashSimulatedState(), simulatedAfter);
        const std::vector<Vector3> poses = Poses(bodies);
        for (std::size_t i = 0; i < poses.size(); ++i)
        {
            EXPECT_EQ(poses[i].x, posesAfter[i].x) << bodies[i]->GetName();
            EXPECT_EQ(poses[i].y, posesAfter[i].y) << bodies[i]->GetName();
        }
        std::size_t next = 0;
        for (const auto &body : bodies)
        {
            if (const auto *joint = body->GetComponent<Joint2D>())
            {
                EXPECT_EQ(joint->GetHandle(), handles[next++]) << body->GetName() << ": claimed back, not made again";
            }
        }
    }
}

TEST_F(Joint2DTest, AJointMadeOrEndedInsideTheWindowIsPutRightByTheRestore)
{
    const auto bob = Body("Bob", 2.0f, 0.0f);
    const auto late = Body("Late", 6.0f, 0.0f);
    Joint2D *kept = ToWorld(bob, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    Step(10);
    Physics2DWorld *const world = _scene->GetPhysics2DWorld();
    std::vector<std::byte> saved;
    ASSERT_TRUE(world->SaveState(saved));

    // Inside the window: the kept joint's component goes, and a new one is made on another body
    bob->RemoveComponent(kept);
    Joint2D *added = ToWorld(late, JointType2D::Hinge, Vector2(4.0f, 0.0f));
    Step(5);
    ASSERT_TRUE(added->IsCreated());

    // The game puts its components back as they were at the save, then the restore and a step
    added->SetActive(false);
    kept = ToWorld(bob, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    ASSERT_TRUE(world->RestoreState(saved));
    Step();
    EXPECT_TRUE(kept->IsCreated()) << "the saved joint, claimed by the component that now has its place";
    EXPECT_FALSE(added->IsCreated());
    Step(40);
    EXPECT_NEAR(Distance(At(bob), Vector2(0.0f, 0.0f)), 2.0f, 0.05f) << "still hinged";
    EXPECT_LT(At(late).y, -2.0f) << "the joint made inside the window is gone: it falls";
}

#else

TEST_F(Joint2DTest, WithoutBox2DAJointKeepsItsDataAndDoesNothing)
{
    const auto bob = Body("Bob", 2.0f, 0.0f);
    Joint2D *joint = ToWorld(bob, JointType2D::Hinge, Vector2(0.0f, 0.0f));
    joint->SetLimits(true, -30.0f, 30.0f);
    Step(3);
    EXPECT_FALSE(joint->IsCreated());
    EXPECT_FLOAT_EQ(joint->GetLimitMax(), 30.0f);
    EXPECT_FLOAT_EQ(joint->GetJointAngle(), 0.0f);
}

#endif
