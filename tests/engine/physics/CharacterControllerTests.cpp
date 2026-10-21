// The engine-owned character controller (netcode B5), run as TYPED tests over every backend this build has (PhysX and
// Jolt): CharacterMotor::Step as a pure function over the backend's capsule casts (flat ground, walking, steps, slopes,
// walls, falling and jumping, ceilings, layers, determinism, save and restore, the corner cases) and the
// CharacterController component around it (the object follows the state, held input, SetState, settings, saving).
//
// The scenes are built from static boxes. The ground's top is y = 0, so a capsule of the default radius 0.5 and
// height 2 rests with its centre at 1 + the skin width (1.02). Where the two backends differ in what their sweeps
// report (a few millimetres), the bounds are wide enough for both; the tests check what the character does, not
// the last digit.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Layers.hpp"
#include "engine/Time.hpp"
#include "engine/physics/CharacterController.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kDt = 1.0f / 60.0f;
        constexpr float kRestHeight = 1.02f; // centre height of the default capsule resting on y = 0
        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
        constexpr float kInf = std::numeric_limits<float>::infinity();

        Math::Quaternion AboutZ(const float degrees)
        {
            return Math::Quaternion::FromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), degrees * 3.14159265358979f / 180.0f);
        }

        bool SameState(const CharacterState &a, const CharacterState &b)
        {
            return CharacterMotor::Hash(a) == CharacterMotor::Hash(b);
        }

        // What the scripted determinism run asks of the character on tick i: it paces back and forth along x (6.7 m
        // each way), drifts in z and jumps now and then
        CharacterInput ScriptedInput(const int i)
        {
            CharacterInput input;
            const float direction = ((i / 100) % 2 == 0) ? 1.0f : -1.0f;
            input.move = Vector3(4.0f * direction, 0.0f, 1.5f);
            input.jump = (i % 50) == 0;
            return input;
        }
    }

    template <typename Tag>
    class CharacterMotorTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const Vector3 &size,
                                 const Math::Quaternion &rotation = Math::Quaternion::Identity)
        {
            const auto go = this->Spawn(name, position);
            go->GetPositionable()->SetRotation(rotation);
            go->template AddComponent<BoxCollider>()->SetSize(size);
            return go;
        }

        // A 60 x 60 ground whose top is y = 0
        void SpawnGround() { SpawnBox("Ground", Vector3(0.0f, -0.5f, 0.0f), Vector3(60.0f, 1.0f, 60.0f)); }

        // A ramp rising along +x at `degrees`, its surface starting at (startX, 0, 0) and 20 long
        void SpawnRamp(const float degrees, const float startX)
        {
            const float radians = degrees * 3.14159265358979f / 180.0f;
            const float c = std::cos(radians);
            const float s = std::sin(radians);
            // The surface point (startX, 0), half a thickness (0.5) below it along the normal, then half the length
            // (10) up the slope
            const Vector3 center(startX + 0.5f * s + 10.0f * c, -0.5f * c + 10.0f * s, 0.0f);
            SpawnBox("Ramp", center, Vector3(20.0f, 1.0f, 60.0f), AboutZ(degrees));
        }

        // Builds the scene's bodies (they are created when the attach queue runs)
        void Build() { this->Step(); }

        CharacterState StateAt(const Vector3 &position) const
        {
            CharacterState state;
            state.position = position;
            return state;
        }

        CharacterState Run(const CharacterSettings &settings, CharacterState state, const CharacterInput &input,
                           const int ticks)
        {
            for (int i = 0; i < ticks; ++i)
            {
                state = CharacterMotor::Step(this->BackendRef(), settings, state, input, kDt);
            }
            return state;
        }

        // Falls from `position` onto the ground and settles
        CharacterState Settled(const CharacterSettings &settings, const Vector3 &position)
        {
            return Run(settings, StateAt(position), CharacterInput{}, 150);
        }
    };

    TYPED_TEST_SUITE(CharacterMotorTest, AllBackends, BackendNames);

    // ============================================================================
    // Ground
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, FallsAndLandsOnFlatGround)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->StateAt(Vector3(0.0f, 3.0f, 0.0f));
        state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
        EXPECT_FALSE(state.grounded);
        EXPECT_LT(state.velocity.y, 0.0f) << "gravity pulls it down";
        EXPECT_LT(state.position.y, 3.0f);

        state = this->Run(settings, state, CharacterInput{}, 150);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, kRestHeight, 0.04f);
        EXPECT_NEAR(state.position.x, 0.0f, 1e-4f);
        EXPECT_NEAR(state.position.z, 0.0f, 1e-4f);
        EXPECT_EQ(state.velocity.y, 0.0f);
        EXPECT_NEAR(state.groundNormal.y, 1.0f, 1e-3f);
    }

    TYPED_TEST(CharacterMotorTest, StandingStillStaysPut)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        const CharacterState settled = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(settled.grounded);
        CharacterState state = settled;
        for (int i = 0; i < 120; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
            ASSERT_TRUE(state.grounded) << "tick " << i;
        }
        ExpectVectorNear(state.position, settled.position, 1e-3f);
    }

    TYPED_TEST(CharacterMotorTest, WalksOnFlatGroundAtTheWantedSpeedAndStaysGrounded)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(state.grounded);
        CharacterInput input;
        input.move = Vector3(3.0f, 0.0f, 0.0f);
        const float startX = state.position.x;
        for (int i = 0; i < 60; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, input, kDt);
            ASSERT_TRUE(state.grounded) << "tick " << i;
            ASSERT_NEAR(state.position.y, kRestHeight, 0.04f) << "tick " << i;
        }
        EXPECT_NEAR(state.position.x - startX, 3.0f, 0.02f) << "a second at 3 m/s";
        EXPECT_NEAR(state.velocity.x, 3.0f, 0.05f) << "the achieved horizontal velocity";
        EXPECT_NEAR(state.position.z, 0.0f, 1e-4f);
    }

    TYPED_TEST(CharacterMotorTest, TheMoveIsHorizontalWhateverItsYIs)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        const CharacterState settled = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput input;
        input.move = Vector3(3.0f, 50.0f, 0.0f); // the y asks to fly: ignored
        const CharacterState state = this->Run(settings, settled, input, 30);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, settled.position.y, 0.04f);
        EXPECT_NEAR(state.position.x - settled.position.x, 1.5f, 0.02f);
    }

    // ============================================================================
    // Jumping and falling
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, AJumpRisesAndComesDownOnTheSameSpot)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings; // jump speed 5, gravity 9.81: a rise of about 1.27 m, 61 ticks in the air

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(state.grounded);
        const float restY = state.position.y;

        CharacterInput jump;
        jump.jump = true;
        state = CharacterMotor::Step(this->BackendRef(), settings, state, jump, kDt);
        EXPECT_FALSE(state.grounded);
        EXPECT_GT(state.velocity.y, 4.0f);

        float highest = state.position.y;
        int airborne = 1;
        for (int i = 0; i < 150; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
            highest = std::max(highest, state.position.y);
            if (!state.grounded)
            {
                ++airborne;
            }
        }
        EXPECT_NEAR(highest - restY, 1.27f, 0.12f);
        EXPECT_NEAR(static_cast<float>(airborne), 61.0f, 5.0f);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, restY, 0.04f);
    }

    TYPED_TEST(CharacterMotorTest, OnlyAGroundedCharacterJumps)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        CharacterInput jump;
        jump.jump = true;
        // In the air the jump does nothing: the same fall as without it
        const CharacterState start = this->StateAt(Vector3(0.0f, 5.0f, 0.0f));
        const CharacterState with = CharacterMotor::Step(this->BackendRef(), settings, start, jump, kDt);
        const CharacterState without = CharacterMotor::Step(this->BackendRef(), settings, start, CharacterInput{}, kDt);
        EXPECT_TRUE(SameState(with, without));
    }

    TYPED_TEST(CharacterMotorTest, ACeilingStopsTheRise)
    {
        this->SpawnGround();
        this->SpawnBox("Ceiling", Vector3(0.0f, 3.0f, 0.0f), Vector3(20.0f, 1.0f, 20.0f)); // underside at y = 2.5
        this->Build();
        CharacterSettings settings;
        settings.jumpSpeed = 8.0f; // would rise 3.3 m

        CharacterState state = this->Settled(settings, Vector3(0.0f, 1.5f, 0.0f));
        ASSERT_TRUE(state.grounded);
        CharacterInput jump;
        jump.jump = true;
        state = CharacterMotor::Step(this->BackendRef(), settings, state, jump, kDt);
        float highest = state.position.y;
        for (int i = 0; i < 40; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
            highest = std::max(highest, state.position.y);
        }
        // The top of the capsule (centre + 1) stops under the ceiling
        EXPECT_LT(highest, 1.5f) << "the head is at 2.5";
        EXPECT_GT(highest, 1.3f) << "but it did rise";
        for (int i = 0; i < 150; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
        }
        EXPECT_TRUE(state.grounded) << "it comes back down";
    }

    TYPED_TEST(CharacterMotorTest, FallingIsCappedAtTheMaxFallSpeed)
    {
        // No ground: a long fall
        this->Build();
        CharacterSettings settings;
        settings.maxFallSpeed = 20.0f;
        CharacterState state = this->StateAt(Vector3(0.0f, 100.0f, 0.0f));
        state = this->Run(settings, state, CharacterInput{}, 240);
        EXPECT_FALSE(state.grounded);
        EXPECT_NEAR(state.velocity.y, -20.0f, 1e-3f);
    }

    TYPED_TEST(CharacterMotorTest, GravityScaleScalesTheFall)
    {
        this->Build();
        CharacterSettings light;
        light.gravityScale = 0.0f;
        const CharacterState floating = this->Run(light, this->StateAt(Vector3(0.0f, 50.0f, 0.0f)), CharacterInput{}, 60);
        EXPECT_NEAR(floating.position.y, 50.0f, 1e-4f) << "no gravity, no fall";

        CharacterSettings reversed;
        reversed.gravityScale = -1.0f;
        const CharacterState rising = this->Run(reversed, this->StateAt(Vector3(0.0f, 50.0f, 0.0f)), CharacterInput{}, 60);
        EXPECT_GT(rising.position.y, 50.0f) << "negative gravity lifts it";
    }

    // ============================================================================
    // Walls
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, AWallStopsItAndTheMoveSlidesAlongIt)
    {
        this->SpawnGround();
        this->SpawnBox("Wall", Vector3(5.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f)); // face at x = 4.5
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput into;
        into.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, into, 150);
        EXPECT_GT(state.position.x, 3.85f);
        EXPECT_LT(state.position.x, 4.0f + 1e-3f) << "stopped by the wall: its face is at 4.5, the radius is 0.5";
        EXPECT_TRUE(state.grounded);

        // Diagonally into the wall: x stays, z carries on
        CharacterInput diagonal;
        diagonal.move = Vector3(3.0f, 0.0f, 3.0f);
        const float startZ = state.position.z;
        for (int i = 0; i < 60; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, diagonal, kDt);
            ASSERT_LT(state.position.x, 4.0f + 1e-3f) << "tick " << i;
        }
        EXPECT_NEAR(state.position.z - startZ, 3.0f, 0.2f) << "it slid along the wall at the speed of its z part";
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, ACornerHoldsItWithoutPushingThrough)
    {
        this->SpawnGround();
        this->SpawnBox("WallX", Vector3(5.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f));
        this->SpawnBox("WallZ", Vector3(0.0f, 2.0f, 5.0f), Vector3(60.0f, 4.0f, 1.0f));
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput corner;
        corner.move = Vector3(4.0f, 0.0f, 4.0f);
        for (int i = 0; i < 300; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, corner, kDt);
            ASSERT_LT(state.position.x, 4.0f + 1e-3f) << "tick " << i;
            ASSERT_LT(state.position.z, 4.0f + 1e-3f) << "tick " << i;
        }
        EXPECT_GT(state.position.x, 3.8f);
        EXPECT_GT(state.position.z, 3.8f);
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, TriggersAreWalkedThrough)
    {
        this->SpawnGround();
        const auto zone = this->SpawnBox("Zone", Vector3(5.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f));
        zone->template GetComponent<BoxCollider>()->SetIsTrigger(true);
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput into;
        into.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, into, 150);
        EXPECT_GT(state.position.x, 6.0f);
    }

    // ============================================================================
    // Steps
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, ClimbsAStepWithinTheStepOffset)
    {
        this->SpawnGround();
        this->SpawnBox("Step", Vector3(7.0f, 0.1f, 0.0f), Vector3(10.0f, 0.2f, 60.0f)); // x 2 to 12, top at y = 0.2
        this->Build();
        const CharacterSettings settings; // step offset 0.3

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        float highest = state.position.y;
        for (int i = 0; i < 120; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, walk, kDt);
            highest = std::max(highest, state.position.y);
        }
        EXPECT_GT(state.position.x, 5.5f) << "it walked on over the step";
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, kRestHeight + 0.2f, 0.05f) << "and stands on top of it";
        EXPECT_LT(highest, kRestHeight + 0.2f + 0.35f) << "without leaping";
    }

    TYPED_TEST(CharacterMotorTest, DoesNotClimbAStepTallerThanTheStepOffset)
    {
        this->SpawnGround();
        this->SpawnBox("Tall", Vector3(7.0f, 0.25f, 0.0f), Vector3(10.0f, 0.5f, 60.0f)); // x 2 to 12, top at y = 0.5
        this->Build();
        const CharacterSettings settings; // step offset 0.3

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, walk, 150);
        EXPECT_LT(state.position.x, 1.7f) << "stopped at the face of the step (x = 2)";
        EXPECT_NEAR(state.position.y, kRestHeight, 0.05f);
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, ARaisedStepOffsetLetsItClimbTheTallerStep)
    {
        this->SpawnGround();
        this->SpawnBox("Tall", Vector3(7.0f, 0.25f, 0.0f), Vector3(10.0f, 0.5f, 60.0f));
        this->Build();
        CharacterSettings settings;
        settings.stepOffset = 0.6f;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, walk, 120);
        EXPECT_GT(state.position.x, 5.5f);
        EXPECT_NEAR(state.position.y, kRestHeight + 0.5f, 0.05f);
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, WalksDownAStepAndStaysGrounded)
    {
        // A platform 0.2 high from x = -20 to x = 2, ground beyond it
        this->SpawnGround();
        this->SpawnBox("Platform", Vector3(-9.0f, 0.1f, 0.0f), Vector3(22.0f, 0.2f, 60.0f));
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_NEAR(state.position.y, kRestHeight + 0.2f, 0.05f);
        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        for (int i = 0; i < 90; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, walk, kDt);
            ASSERT_TRUE(state.grounded) << "tick " << i << ": a step down is within the step offset";
        }
        EXPECT_GT(state.position.x, 3.0f);
        EXPECT_NEAR(state.position.y, kRestHeight, 0.05f);
    }

    TYPED_TEST(CharacterMotorTest, WalksOffALedgeAndFalls)
    {
        // A platform 2 high up to x = 2: stepping off it is a fall, not a step down
        this->SpawnGround();
        this->SpawnBox("Platform", Vector3(-9.0f, 1.0f, 0.0f), Vector3(22.0f, 2.0f, 60.0f));
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->Settled(settings, Vector3(0.0f, 5.0f, 0.0f));
        ASSERT_NEAR(state.position.y, kRestHeight + 2.0f, 0.05f);
        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        bool wasAirborne = false;
        for (int i = 0; i < 150; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, walk, kDt);
            wasAirborne = wasAirborne || !state.grounded;
        }
        EXPECT_TRUE(wasAirborne);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, kRestHeight, 0.05f) << "it ended on the ground";
    }

    // ============================================================================
    // Slopes
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, WalksUpAndDownAWalkableSlope)
    {
        this->SpawnGround();
        this->SpawnRamp(20.0f, 3.0f);
        this->Build();
        const CharacterSettings settings; // slope limit 45

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(state.grounded);
        CharacterInput up;
        up.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, up, 180);
        EXPECT_TRUE(state.grounded);
        EXPECT_GT(state.position.x, 7.0f);
        EXPECT_GT(state.position.y, 2.0f) << "it climbed";
        EXPECT_NEAR(state.groundNormal.y, std::cos(20.0f * 3.14159265f / 180.0f), 0.03f);

        // And back down: it follows the slope, grounded at every tick
        CharacterInput down;
        down.move = Vector3(-3.0f, 0.0f, 0.0f);
        for (int i = 0; i < 90; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, down, kDt);
            ASSERT_TRUE(state.grounded) << "tick " << i << ": it sticks to the slope going down";
        }
        EXPECT_LT(state.position.x, 7.0f);
    }

    TYPED_TEST(CharacterMotorTest, DoesNotClimbASlopeSteeperThanTheLimit)
    {
        this->SpawnGround();
        this->SpawnRamp(60.0f, 3.0f);
        this->Build();
        const CharacterSettings settings; // slope limit 45

        CharacterState state = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(state.grounded);
        CharacterInput up;
        up.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, up, 200);
        EXPECT_LT(state.position.y, 1.5f) << "it stayed on the flat ground at the foot of the slope";
        EXPECT_LT(state.position.x, 4.5f);
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, ARaisedSlopeLimitLetsItClimbTheSteepSlope)
    {
        this->SpawnGround();
        this->SpawnRamp(35.0f, 3.0f);
        this->Build();
        CharacterSettings strict;
        strict.slopeLimit = 30.0f;
        CharacterSettings lenient;
        lenient.slopeLimit = 40.0f;

        CharacterInput up;
        up.move = Vector3(3.0f, 0.0f, 0.0f);
        const CharacterState blocked = this->Run(strict, this->Settled(strict, Vector3(0.0f, 3.0f, 0.0f)), up, 200);
        EXPECT_LT(blocked.position.y, 1.6f) << "35 degrees is over a limit of 30";
        const CharacterState climbed = this->Run(lenient, this->Settled(lenient, Vector3(0.0f, 3.0f, 0.0f)), up, 200);
        EXPECT_GT(climbed.position.y, 2.5f) << "and under a limit of 40";
    }

    TYPED_TEST(CharacterMotorTest, ASteepSlopeIsNotGroundToStandOn)
    {
        // Dropped onto the face of a 60 degree ramp: it does not stand there, it ends up sliding off (on the ground
        // beside it, in the end)
        this->SpawnGround();
        this->SpawnRamp(60.0f, 3.0f);
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->StateAt(Vector3(8.0f, 12.0f, 0.0f));
        for (int i = 0; i < 120; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
            if (state.grounded)
            {
                // A ground is only ever reported with a normal within the limit (cos 45 = 0.707)
                ASSERT_GT(state.groundNormal.y, 0.69f) << "tick " << i;
            }
            ASSERT_LT(state.position.x, 8.5f) << "tick " << i << ": it does not climb";
        }
    }

    // ============================================================================
    // Layers and triggers
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, ItCollidesOnlyWithItsLayerMask)
    {
        this->SpawnGround();
        this->Build();

        CharacterSettings none;
        none.layerMask = Layers::MaskOf(8); // the ground is on layer 0
        const CharacterState fell = this->Run(none, this->StateAt(Vector3(0.0f, 3.0f, 0.0f)), CharacterInput{}, 120);
        EXPECT_LT(fell.position.y, -3.0f) << "fell through";
        EXPECT_FALSE(fell.grounded);

        CharacterSettings all;
        all.layerMask = Layers::MaskOf(Layers::Default);
        const CharacterState stood = this->Run(all, this->StateAt(Vector3(0.0f, 3.0f, 0.0f)), CharacterInput{}, 120);
        EXPECT_TRUE(stood.grounded);

        CharacterSettings nothing;
        nothing.layerMask = 0u;
        const CharacterState nowhere = this->Run(nothing, this->StateAt(Vector3(0.0f, 3.0f, 0.0f)), CharacterInput{}, 120);
        EXPECT_FALSE(nowhere.grounded);
        EXPECT_LT(nowhere.position.y, -3.0f);
    }

    // ============================================================================
    // Determinism, save and restore
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, TheSameInputsGiveTheSameStatesTwice)
    {
        this->SpawnGround();
        this->SpawnBox("Step", Vector3(3.0f, 0.1f, 0.0f), Vector3(2.0f, 0.2f, 60.0f));  // x 2 to 4
        this->SpawnBox("WallX", Vector3(7.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f)); // face at x = 6.5
        this->SpawnBox("WallZ", Vector3(0.0f, 2.0f, 6.0f), Vector3(60.0f, 4.0f, 1.0f)); // face at z = 5.5
        this->Build();
        const CharacterSettings settings;

        std::vector<std::uint64_t> first;
        std::vector<std::uint64_t> second;
        for (std::vector<std::uint64_t> *run : {&first, &second})
        {
            CharacterState state = this->StateAt(Vector3(0.0f, 2.0f, 0.0f));
            for (int i = 0; i < 400; ++i)
            {
                state = CharacterMotor::Step(this->BackendRef(), settings, state, ScriptedInput(i), kDt);
                run->push_back(CharacterMotor::Hash(state));
            }
        }
        ASSERT_EQ(first.size(), 400u);
        EXPECT_EQ(first, second) << "the hash of the state matched on every tick";
    }

    TYPED_TEST(CharacterMotorTest, ARestoredStateReplaysTheSameFuture)
    {
        this->SpawnGround();
        this->SpawnBox("Step", Vector3(3.0f, 0.1f, 0.0f), Vector3(2.0f, 0.2f, 60.0f));
        this->SpawnBox("WallX", Vector3(7.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f));
        this->Build();
        const CharacterSettings settings;

        CharacterState state = this->StateAt(Vector3(0.0f, 2.0f, 0.0f));
        CharacterState saved;
        std::vector<std::uint64_t> future;
        for (int i = 0; i < 250; ++i)
        {
            if (i == 100)
            {
                saved = state; // a copy is the whole save
            }
            state = CharacterMotor::Step(this->BackendRef(), settings, state, ScriptedInput(i), kDt);
            if (i >= 100)
            {
                future.push_back(CharacterMotor::Hash(state));
            }
        }

        // Back to tick 100 and the same inputs: the same states
        state = saved;
        std::vector<std::uint64_t> replay;
        for (int i = 100; i < 250; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, ScriptedInput(i), kDt);
            replay.push_back(CharacterMotor::Hash(state));
        }
        EXPECT_EQ(future, replay);
    }

    TYPED_TEST(CharacterMotorTest, TheHashSeesEveryFieldOfTheState)
    {
        CharacterState base;
        const std::uint64_t reference = CharacterMotor::Hash(base);
        EXPECT_EQ(reference, CharacterMotor::Hash(CharacterState{}));

        CharacterState changed = base;
        changed.position.x = 1.0f;
        EXPECT_NE(CharacterMotor::Hash(changed), reference);
        changed = base;
        changed.position.z = 1.0f;
        EXPECT_NE(CharacterMotor::Hash(changed), reference);
        changed = base;
        changed.velocity.y = 1.0f;
        EXPECT_NE(CharacterMotor::Hash(changed), reference);
        changed = base;
        changed.grounded = true;
        EXPECT_NE(CharacterMotor::Hash(changed), reference);
        changed = base;
        changed.groundNormal.x = 0.5f;
        EXPECT_NE(CharacterMotor::Hash(changed), reference);
    }

    // ============================================================================
    // Corner cases
    // ============================================================================

    TYPED_TEST(CharacterMotorTest, ABadDtOrStateOrSettingsChangesNothing)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;
        const CharacterState state = this->StateAt(Vector3(1.0f, 3.0f, 2.0f));
        CharacterInput walk;
        walk.move = Vector3(1.0f, 0.0f, 0.0f);

        for (const float dt : {0.0f, -kDt, kNaN, kInf})
        {
            EXPECT_TRUE(SameState(CharacterMotor::Step(this->BackendRef(), settings, state, walk, dt), state)) << dt;
        }

        CharacterState broken = state;
        broken.position.y = kNaN;
        EXPECT_TRUE(SameState(CharacterMotor::Step(this->BackendRef(), settings, broken, walk, kDt), broken));
        broken.position = Vector3(kInf, 0.0f, 0.0f);
        EXPECT_TRUE(SameState(CharacterMotor::Step(this->BackendRef(), settings, broken, walk, kDt), broken));

        CharacterSettings bad = settings;
        bad.radius = 0.0f;
        EXPECT_FALSE(bad.IsValid());
        EXPECT_TRUE(SameState(CharacterMotor::Step(this->BackendRef(), bad, state, walk, kDt), state));
        bad = settings;
        bad.slopeLimit = 90.0f;
        EXPECT_FALSE(bad.IsValid());
        bad = settings;
        bad.skinWidth = kNaN;
        EXPECT_FALSE(bad.IsValid());
        bad = settings;
        bad.height = -1.0f;
        EXPECT_FALSE(bad.IsValid());
        EXPECT_TRUE(settings.IsValid());
    }

    TYPED_TEST(CharacterMotorTest, ANonFiniteMoveCountsAsNone)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;
        const CharacterState settled = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));

        const CharacterState still = CharacterMotor::Step(this->BackendRef(), settings, settled, CharacterInput{}, kDt);
        for (const Vector3 &move : {Vector3(kNaN, 0.0f, 0.0f), Vector3(0.0f, 0.0f, kInf), Vector3(kNaN, kNaN, kNaN)})
        {
            CharacterInput input;
            input.move = move;
            const CharacterState state = CharacterMotor::Step(this->BackendRef(), settings, settled, input, kDt);
            EXPECT_TRUE(SameState(state, still));
            EXPECT_TRUE(std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
                        std::isfinite(state.position.z));
        }
    }

    TYPED_TEST(CharacterMotorTest, AnAbsurdMoveIsCutToTheMaximumSpeed)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;
        const CharacterState settled = this->Settled(settings, Vector3(0.0f, 3.0f, 0.0f));

        CharacterInput input;
        input.move = Vector3(1.0e5f, 0.0f, 0.0f);
        const CharacterState state = CharacterMotor::Step(this->BackendRef(), settings, settled, input, kDt);
        EXPECT_TRUE(std::isfinite(state.position.x));
        EXPECT_NEAR(state.position.x - settled.position.x, CharacterMotor::kMaxMoveSpeed * kDt, 0.05f);

        input.move = Vector3(1.0e30f, 0.0f, 0.0f); // its square is not even a float: still cut
        const CharacterState huge = CharacterMotor::Step(this->BackendRef(), settings, settled, input, kDt);
        EXPECT_TRUE(std::isfinite(huge.position.x));
        EXPECT_NEAR(huge.position.x - settled.position.x, CharacterMotor::kMaxMoveSpeed * kDt, 0.05f);
    }

    TYPED_TEST(CharacterMotorTest, ATinyCapsuleLandsAndWalks)
    {
        this->SpawnGround();
        this->SpawnBox("Wall", Vector3(5.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f));
        this->Build();
        CharacterSettings settings;
        settings.radius = 0.01f;
        settings.height = 0.02f; // a sphere
        settings.stepOffset = 0.005f;

        CharacterState state = this->Run(settings, this->StateAt(Vector3(0.0f, 1.0f, 0.0f)), CharacterInput{}, 200);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, 0.01f, 0.02f);
        EXPECT_GE(state.position.y, 0.0f) << "above the ground, not in it";

        CharacterInput walk;
        walk.move = Vector3(1.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, walk, 60);
        EXPECT_GT(state.position.x, 0.9f);
        EXPECT_TRUE(state.grounded);

        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        state = this->Run(settings, state, walk, 200);
        EXPECT_LT(state.position.x, 4.5f) << "and the wall stops even a capsule of a centimetre";
    }

    TYPED_TEST(CharacterMotorTest, AShortCapsuleIsASphere)
    {
        this->SpawnGround();
        this->Build();
        CharacterSettings settings;
        settings.radius = 0.5f;
        settings.height = 0.5f; // under twice the radius
        const CharacterState state = this->Run(settings, this->StateAt(Vector3(0.0f, 2.0f, 0.0f)), CharacterInput{}, 150);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, 0.5f + 0.02f, 0.04f) << "resting on a sphere of radius 0.5";
    }

    TYPED_TEST(CharacterMotorTest, IsPushedOutOfGroundItStartsIn)
    {
        this->SpawnGround();
        this->Build();
        const CharacterSettings settings;

        // The centre at y = 0.2: the lower half of the capsule is inside the ground slab
        CharacterState state = this->StateAt(Vector3(0.0f, 0.2f, 0.0f));
        state = CharacterMotor::Step(this->BackendRef(), settings, state, CharacterInput{}, kDt);
        EXPECT_TRUE(std::isfinite(state.position.y));
        EXPECT_GT(state.position.y, 0.9f) << "moved out of the slab in one step";
        state = this->Run(settings, state, CharacterInput{}, 60);
        EXPECT_TRUE(state.grounded);
        EXPECT_NEAR(state.position.y, kRestHeight, 0.05f);
        EXPECT_NEAR(state.position.x, 0.0f, 0.2f);
        EXPECT_NEAR(state.position.z, 0.0f, 0.2f);
    }

    TYPED_TEST(CharacterMotorTest, IsPushedOutSidewaysFromAWallItStartsIn)
    {
        this->SpawnGround();
        this->SpawnBox("Wall", Vector3(5.0f, 2.0f, 0.0f), Vector3(1.0f, 4.0f, 60.0f)); // x 4.5 to 5.5
        this->Build();
        const CharacterSettings settings;

        // Half in the wall's face, standing on the ground
        CharacterState state = this->StateAt(Vector3(4.3f, kRestHeight, 0.0f));
        state = this->Run(settings, state, CharacterInput{}, 30);
        EXPECT_LT(state.position.x, 4.0f + 1e-3f) << "out of the wall, on the side it was nearest";
        EXPECT_GT(state.position.x, 3.0f);
        EXPECT_TRUE(state.grounded);
    }

    TYPED_TEST(CharacterMotorTest, BuriedTooDeepItStaysAndNothingHangsOrGoesNonFinite)
    {
        // A 100 m solid block: the recovery looks 16 rings out (2 m) and finds no way out
        this->SpawnBox("Block", Vector3(0.0f, 0.0f, 0.0f), Vector3(100.0f, 100.0f, 100.0f));
        this->Build();
        const CharacterSettings settings;

        CharacterInput walk;
        walk.move = Vector3(3.0f, 0.0f, 0.0f);
        CharacterState state = this->StateAt(Vector3(0.0f, 0.0f, 0.0f));
        for (int i = 0; i < 20; ++i)
        {
            state = CharacterMotor::Step(this->BackendRef(), settings, state, walk, kDt);
            ASSERT_TRUE(std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
                        std::isfinite(state.position.z) && std::isfinite(state.velocity.y))
                << "tick " << i;
            ASSERT_LT(std::abs(state.position.x), 50.0f);
            ASSERT_LT(std::abs(state.position.y), 50.0f);
        }
    }

    // ============================================================================
    // The component
    // ============================================================================

    template <typename Tag>
    class CharacterControllerComponentTest : public CharacterMotorTest<Tag>
    {
    protected:
        CharacterController *SpawnCharacter(const Vector3 &position)
        {
            _character = this->Spawn("Character", position);
            return _character->template AddComponent<CharacterController>();
        }

        GameObject::Ptr _character;
    };

    TYPED_TEST_SUITE(CharacterControllerComponentTest, AllBackends, BackendNames);

    TYPED_TEST(CharacterControllerComponentTest, StartsAtTheObjectAndTheObjectFollowsIt)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(2.0f, 3.0f, -1.0f));
        this->Build(); // attaches: the state starts at the object's position
        EXPECT_NEAR(controller->GetState().position.x, 2.0f, 1e-5f);
        EXPECT_NEAR(controller->GetState().position.z, -1.0f, 1e-5f);

        for (int i = 0; i < 150; ++i)
        {
            ASSERT_TRUE(controller->Step(kDt));
        }
        EXPECT_TRUE(controller->GetState().grounded);
        EXPECT_NEAR(controller->GetState().position.y, kRestHeight, 0.05f);
        const Vector3 at = this->_character->GetPositionable()->GetPosition();
        ExpectVectorNear(at, controller->GetState().position, 1e-5f);
    }

    TYPED_TEST(CharacterControllerComponentTest, TheFixedTickMovesIt)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();
        const float before = controller->GetState().position.y;
        this->Step(10); // OnFixedUpdate, with the fixed time step of the process
        if (Time::GetFixedDeltaTime() > 0.0f)
        {
            EXPECT_LT(controller->GetState().position.y, before) << "it fell";
        }
    }

    TYPED_TEST(CharacterControllerComponentTest, TheMoveIsHeldAndTheJumpIsOnePress)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();
        for (int i = 0; i < 150; ++i)
        {
            controller->Step(kDt);
        }
        ASSERT_TRUE(controller->GetState().grounded);

        CharacterInput input;
        input.move = Vector3(3.0f, 0.0f, 0.0f);
        input.jump = true;
        ASSERT_TRUE(controller->SetInput(input));
        const float startX = controller->GetState().position.x;
        controller->Step(kDt);
        EXPECT_FALSE(controller->GetState().grounded) << "it jumped";
        EXPECT_FALSE(controller->GetInput().jump) << "the press is used up";
        EXPECT_NEAR(controller->GetInput().move.x, 3.0f, 0.0f) << "the move stays";
        for (int i = 0; i < 59; ++i)
        {
            controller->Step(kDt);
        }
        EXPECT_NEAR(controller->GetState().position.x - startX, 3.0f, 0.1f) << "and keeps moving it";
    }

    TYPED_TEST(CharacterControllerComponentTest, ANonFiniteInputIsRefused)
    {
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();
        CharacterInput good;
        good.move = Vector3(1.0f, 0.0f, 0.0f);
        ASSERT_TRUE(controller->SetInput(good));
        CharacterInput bad;
        bad.move = Vector3(kNaN, 0.0f, 0.0f);
        EXPECT_FALSE(controller->SetInput(bad));
        EXPECT_NEAR(controller->GetInput().move.x, 1.0f, 0.0f) << "the old input stays";
    }

    TYPED_TEST(CharacterControllerComponentTest, SetStateRestoresAndMovesTheObject)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();

        CharacterState wanted;
        wanted.position = Vector3(4.0f, 7.0f, -2.0f);
        wanted.velocity = Vector3(1.0f, -3.0f, 0.5f);
        wanted.grounded = false;
        ASSERT_TRUE(controller->SetState(wanted));
        EXPECT_TRUE(SameState(controller->GetState(), wanted));
        ExpectVectorNear(this->_character->GetPositionable()->GetPosition(), wanted.position, 1e-5f);

        // Saved and restored later: the same future
        const CharacterState saved = controller->GetState();
        for (int i = 0; i < 40; ++i)
        {
            controller->Step(kDt);
        }
        const std::uint64_t future = CharacterMotor::Hash(controller->GetState());
        ASSERT_TRUE(controller->SetState(saved));
        for (int i = 0; i < 40; ++i)
        {
            controller->Step(kDt);
        }
        EXPECT_EQ(CharacterMotor::Hash(controller->GetState()), future);
    }

    TYPED_TEST(CharacterControllerComponentTest, ABadStateIsRefusedAndChangesNothing)
    {
        auto *controller = this->SpawnCharacter(Vector3(1.0f, 2.0f, 3.0f));
        this->Build();
        const CharacterState before = controller->GetState();

        CharacterState bad;
        bad.position = Vector3(kNaN, 0.0f, 0.0f);
        EXPECT_FALSE(controller->SetState(bad));
        bad.position = Vector3(0.0f, 0.0f, 0.0f);
        bad.velocity = Vector3(0.0f, kInf, 0.0f);
        EXPECT_FALSE(controller->SetState(bad));
        bad.velocity = Vector3(0.0f, 0.0f, 0.0f);
        bad.groundNormal = Vector3(kNaN, 1.0f, 0.0f);
        EXPECT_FALSE(controller->SetState(bad));
        EXPECT_FALSE(controller->Teleport(Vector3(0.0f, kNaN, 0.0f)));
        EXPECT_TRUE(SameState(controller->GetState(), before));

        // A ground normal of no length becomes up
        CharacterState flat;
        flat.position = Vector3(0.0f, 5.0f, 0.0f);
        flat.groundNormal = Vector3(0.0f, 0.0f, 0.0f);
        ASSERT_TRUE(controller->SetState(flat));
        EXPECT_NEAR(controller->GetState().groundNormal.y, 1.0f, 0.0f);
    }

    TYPED_TEST(CharacterControllerComponentTest, TeleportPutsItAtRestInTheAir)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();
        for (int i = 0; i < 150; ++i)
        {
            controller->Step(kDt);
        }
        ASSERT_TRUE(controller->GetState().grounded);

        ASSERT_TRUE(controller->Teleport(Vector3(6.0f, 4.0f, 6.0f)));
        EXPECT_FALSE(controller->GetState().grounded);
        EXPECT_EQ(controller->GetState().velocity.y, 0.0f);
        ExpectVectorNear(controller->GetState().position, Vector3(6.0f, 4.0f, 6.0f), 0.0f);
        for (int i = 0; i < 150; ++i)
        {
            controller->Step(kDt);
        }
        EXPECT_TRUE(controller->GetState().grounded);
        EXPECT_NEAR(controller->GetState().position.x, 6.0f, 1e-3f);
    }

    TYPED_TEST(CharacterControllerComponentTest, InvalidSettingsMeanItDoesNotMove)
    {
        this->SpawnGround();
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 3.0f, 0.0f));
        this->Build();
        const float before = controller->GetState().position.y;
        controller->radius = 0.0f;
        EXPECT_FALSE(controller->Step(kDt));
        EXPECT_EQ(controller->GetState().position.y, before);
        controller->radius = 0.5f;
        EXPECT_TRUE(controller->Step(kDt));
        EXPECT_FALSE(controller->Step(0.0f)) << "a dt of 0 does not run";
        EXPECT_FALSE(controller->Step(kNaN));
    }

    TYPED_TEST(CharacterControllerComponentTest, ItsSettingsAreSavedWithTheScene)
    {
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 0.0f, 0.0f));
        controller->radius = 0.3f;
        controller->height = 1.5f;
        controller->stepOffset = 0.4f;
        controller->slopeLimit = 50.0f;
        controller->skinWidth = 0.03f;
        controller->jumpSpeed = 7.0f;
        controller->gravityScale = 2.0f;
        controller->maxFallSpeed = 30.0f;
        controller->layerMask = static_cast<int>(Layers::MaskOf(Layers::Default) | Layers::MaskOf(8));

        const nlohmann::json saved = controller->Serialize();
        const auto other = this->Spawn("Other", Vector3(0.0f, 0.0f, 0.0f));
        auto *copy = other->template AddComponent<CharacterController>();
        copy->Deserialize(saved);
        EXPECT_NEAR(copy->radius, 0.3f, 1e-6f);
        EXPECT_NEAR(copy->height, 1.5f, 1e-6f);
        EXPECT_NEAR(copy->stepOffset, 0.4f, 1e-6f);
        EXPECT_NEAR(copy->slopeLimit, 50.0f, 1e-6f);
        EXPECT_NEAR(copy->skinWidth, 0.03f, 1e-6f);
        EXPECT_NEAR(copy->jumpSpeed, 7.0f, 1e-6f);
        EXPECT_NEAR(copy->gravityScale, 2.0f, 1e-6f);
        EXPECT_NEAR(copy->maxFallSpeed, 30.0f, 1e-6f);
        EXPECT_EQ(copy->layerMask, controller->layerMask);

        const CharacterSettings settings = copy->GetSettings();
        EXPECT_TRUE(settings.IsValid());
        EXPECT_EQ(settings.layerMask, Layers::MaskOf(Layers::Default) | Layers::MaskOf(8));
    }

    TYPED_TEST(CharacterControllerComponentTest, ADefaultControllerSettingsAreTheDocumentedOnes)
    {
        auto *controller = this->SpawnCharacter(Vector3(0.0f, 0.0f, 0.0f));
        const CharacterSettings settings = controller->GetSettings();
        EXPECT_NEAR(settings.radius, 0.5f, 0.0f);
        EXPECT_NEAR(settings.height, 2.0f, 0.0f);
        EXPECT_NEAR(settings.stepOffset, 0.3f, 0.0f);
        EXPECT_NEAR(settings.slopeLimit, 45.0f, 0.0f);
        EXPECT_NEAR(settings.skinWidth, 0.02f, 0.0f);
        EXPECT_NEAR(settings.jumpSpeed, 5.0f, 0.0f);
        EXPECT_NEAR(settings.gravityScale, 1.0f, 0.0f);
        EXPECT_NEAR(settings.maxFallSpeed, 50.0f, 0.0f);
        EXPECT_EQ(settings.layerMask, Layers::DefaultRaycastMask);
        EXPECT_TRUE(settings.IsValid());
    }
}

#endif // a physics backend is in this build
