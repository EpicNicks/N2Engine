#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/NetworkPrediction.hpp"

// The pure parts of client prediction (netcode stage B6): the input mapping and the history that compares a server state
// with what was predicted and replays the inputs after it. The stepper here is a scripted one (position moves by the
// input, vertical speed never changes), so every number below is exact and no physics backend is needed. The full
// client, server and character controller over the network simulator are in NetworkPredictionSessionTests.cpp.

using namespace N2Engine;
using namespace N2Engine::Network;
using N2Engine::Physics::CharacterInput;
using N2Engine::Physics::CharacterState;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    InputControlSpec Spec(const std::string &name, const InputControlKind kind, const std::uint8_t bits, const bool isSigned)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = kind;
        spec.bits = bits;
        spec.range = 1.0f;
        spec.isSigned = isSigned;
        return spec;
    }

    /// throttle (axis), move (vector2), jump (button): the layout orders them by name, so the values are jump, move.x, move.y, throttle
    InputLayout Layout()
    {
        return InputLayout::Create({Spec("throttle", InputControlKind::Axis, 6, false),
                                    Spec("move", InputControlKind::Vector2, 8, true),
                                    Spec("jump", InputControlKind::Button, 1, false)})
            .value();
    }

    CharacterInput Move(const float x, const float z = 0.0f, const bool jump = false)
    {
        CharacterInput input;
        input.move = Math::Vector3(x, 0.0f, z);
        input.jump = jump;
        return input;
    }

    CharacterState At(const float x, const float y = 0.0f, const float z = 0.0f)
    {
        CharacterState state;
        state.position = Math::Vector3(x, y, z);
        return state;
    }

    /// position += move * dt; the vertical speed and the grounded flag stay (the history compares those too)
    CharacterState Scripted(const CharacterState &state, const CharacterInput &input, const float dt)
    {
        CharacterState next = state;
        next.position = state.position + input.move * dt;
        return next;
    }

    const PredictionStepper kStepper = &Scripted;

    /// A history that has stepped `count` ticks starting at `first` with `input` each, from x = 0
    PredictionHistory Recorded(const std::uint32_t first, const std::uint32_t count, const CharacterInput &input,
                               const std::size_t capacity = kMaxPredictionHistory)
    {
        PredictionHistory history(capacity);
        CharacterState state = At(0.0f);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            state = Scripted(state, input, kDt);
            EXPECT_TRUE(history.Record(first + i, input, kDt, state));
        }
        return history;
    }

    PredictionOptions Options()
    {
        return PredictionOptions{}.Clamped();
    }
}

// ---- The input mapping -------------------------------------------------------------------------------------------------

TEST(CharacterInputMapTest, MapsMoveAndJumpByControlName)
{
    CharacterInputMapping mapping;
    mapping.moveSpeed = 4.0f;
    const auto map = CharacterInputMap::Create(mapping, Layout());
    ASSERT_TRUE(map.has_value()) << map.error();

    const std::vector<float> values = {1.0f, 0.5f, -1.0f, 0.0f}; // jump, move.x, move.y, throttle (the layout orders by name)
    const CharacterInput input = map->Map(values);
    // (0.5, -1) is longer than 1: cut to length 1 first, then scaled by the speed
    const float length = std::sqrt(0.5f * 0.5f + 1.0f);
    EXPECT_NEAR(input.move.x, 0.5f / length * 4.0f, 1e-5f);
    EXPECT_NEAR(input.move.z, -1.0f / length * 4.0f, 1e-5f);
    EXPECT_EQ(input.move.y, 0.0f);
    EXPECT_TRUE(input.jump);
    EXPECT_NEAR(Math::Vector3(input.move.x, 0.0f, input.move.z).Length(), 4.0f, 1e-4f);
}

TEST(CharacterInputMapTest, ShortDeflectionIsKeptAndAHostileLengthIsCapped)
{
    const auto map = CharacterInputMap::Create({}, Layout());
    ASSERT_TRUE(map.has_value());
    const CharacterInput half = map->Map(std::vector<float>{0.0f, 0.25f, 0.0f, 0.0f});
    EXPECT_NEAR(half.move.x, 0.25f * map->MoveSpeed(), 1e-5f);
    EXPECT_FALSE(half.jump);

    const CharacterInput huge = map->Map(std::vector<float>{0.0f, 1000.0f, 1000.0f, 0.0f});
    EXPECT_NEAR(Math::Vector3(huge.move.x, 0.0f, huge.move.z).Length(), map->MoveSpeed(), 1e-4f);
}

TEST(CharacterInputMapTest, NonFiniteValuesAndShortArraysGiveNoInput)
{
    const auto map = CharacterInputMap::Create({}, Layout());
    ASSERT_TRUE(map.has_value());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const CharacterInput bad = map->Map(std::vector<float>{0.0f, nan, inf, 0.0f});
    EXPECT_EQ(bad.move.x, 0.0f);
    EXPECT_EQ(bad.move.z, 0.0f);

    const CharacterInput fewer = map->Map(std::vector<float>{0.0f, 1.0f});
    EXPECT_EQ(fewer.move.x, 0.0f);
    EXPECT_FALSE(fewer.jump);
    const CharacterInput none = map->Map({});
    EXPECT_EQ(none.move.z, 0.0f);
}

TEST(CharacterInputMapTest, AMissingOrWrongKindControlIsAnErrorThatNamesIt)
{
    CharacterInputMapping mapping;
    mapping.moveControl = "walk";
    const auto missing = CharacterInputMap::Create(mapping, Layout());
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("walk"), std::string::npos);

    mapping.moveControl = "throttle"; // an axis, not a vector2
    const auto wrongMove = CharacterInputMap::Create(mapping, Layout());
    ASSERT_FALSE(wrongMove.has_value());
    EXPECT_NE(wrongMove.error().find("throttle"), std::string::npos);

    mapping = {};
    mapping.jumpControl = "move"; // a vector2, not a button
    const auto wrongJump = CharacterInputMap::Create(mapping, Layout());
    ASSERT_FALSE(wrongJump.has_value());
    EXPECT_NE(wrongJump.error().find("move"), std::string::npos);

    // Empty names switch the control off
    mapping = {};
    mapping.jumpControl.clear();
    const auto noJump = CharacterInputMap::Create(mapping, Layout());
    ASSERT_TRUE(noJump.has_value());
    EXPECT_FALSE(noJump->Map(std::vector<float>{1.0f, 0.0f, 0.0f, 0.0f}).jump);
}

TEST(CharacterInputMapTest, TheSpeedIsClampedAndNonFiniteIsZero)
{
    CharacterInputMapping mapping;
    mapping.moveSpeed = 1.0e9f;
    EXPECT_EQ(CharacterInputMap::Create(mapping, Layout())->MoveSpeed(), Physics::CharacterMotor::kMaxMoveSpeed);
    mapping.moveSpeed = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(CharacterInputMap::Create(mapping, Layout())->MoveSpeed(), 0.0f);
    mapping.moveSpeed = -3.0f;
    EXPECT_EQ(CharacterInputMap::Create(mapping, Layout())->MoveSpeed(), 0.0f);
}

// ---- The options -------------------------------------------------------------------------------------------------------

TEST(PredictionOptionsTest, ClampedBoundsEveryKnob)
{
    PredictionOptions options;
    options.stepSeconds = -1.0f;
    options.positionEpsilon = std::numeric_limits<float>::quiet_NaN();
    options.velocityEpsilon = -5.0f;
    options.snapDistance = std::numeric_limits<float>::infinity();
    options.smoothingMicros = 1ull << 60;
    options.historyTicks = 1;
    PredictionOptions clamped = options.Clamped();
    EXPECT_FLOAT_EQ(clamped.stepSeconds, 1.0f / 60.0f);
    EXPECT_FLOAT_EQ(clamped.positionEpsilon, 0.01f);
    EXPECT_EQ(clamped.velocityEpsilon, 0.0f);
    EXPECT_EQ(clamped.snapDistance, 1.0f);
    EXPECT_EQ(clamped.smoothingMicros, 5'000'000u);
    EXPECT_EQ(clamped.historyTicks, 8u);

    options.historyTicks = 100000;
    EXPECT_EQ(options.Clamped().historyTicks, kMaxPredictionHistory);
}

TEST(PredictionOptionsTest, FromSettingsTakesTickRateSmoothingAndSnapThreshold)
{
    NetworkSettings settings;
    settings.tickRate = 30;
    settings.authoritative.smoothingMs = 250;
    settings.authoritative.snapThreshold = 2.5;
    const PredictionOptions options = PredictionOptions::FromSettings(settings);
    EXPECT_FLOAT_EQ(options.stepSeconds, 1.0f / 30.0f);
    EXPECT_EQ(options.smoothingMicros, 250'000u);
    EXPECT_FLOAT_EQ(options.snapDistance, 2.5f);
    EXPECT_FLOAT_EQ(options.positionEpsilon, 0.01f);
    EXPECT_FALSE(options.enabled) << "settings do not switch prediction on";

    // The defaults of the settings are the defaults of the options
    const PredictionOptions defaults = PredictionOptions::FromSettings(NetworkSettings{});
    EXPECT_EQ(defaults.smoothingMicros, PredictionOptions{}.smoothingMicros);
    EXPECT_FLOAT_EQ(defaults.snapDistance, PredictionOptions{}.snapDistance);
    EXPECT_FLOAT_EQ(defaults.stepSeconds, PredictionOptions{}.stepSeconds);
}

TEST(PredictionOptionsTest, TuningReplacesOnlyItsFieldsAndIsClamped)
{
    PredictionOptions base;
    base.stepSeconds = 1.0f / 30.0f;
    base.historyTicks = 32;
    base.mapping.moveSpeed = 7.0f;

    PredictionTuning tuning = PredictionTuning::Of(base);
    EXPECT_FLOAT_EQ(tuning.positionEpsilon, 0.01f);
    EXPECT_EQ(tuning.smoothingMicros, 100'000u);
    tuning.positionEpsilon = 0.05f;
    tuning.snapDistance = 3.0f;
    tuning.smoothingMicros = 1ull << 60;
    tuning.velocityEpsilon = std::numeric_limits<float>::quiet_NaN();
    tuning.applySmoothing = false;
    const PredictionOptions out = tuning.AppliedTo(base);
    EXPECT_FLOAT_EQ(out.positionEpsilon, 0.05f);
    EXPECT_FLOAT_EQ(out.snapDistance, 3.0f);
    EXPECT_EQ(out.smoothingMicros, 5'000'000u);
    EXPECT_FLOAT_EQ(out.velocityEpsilon, 0.05f) << "not finite: the default";
    EXPECT_FALSE(out.applySmoothing);
    // What the step depends on is untouched
    EXPECT_FLOAT_EQ(out.stepSeconds, 1.0f / 30.0f);
    EXPECT_EQ(out.historyTicks, 32u);
    EXPECT_FLOAT_EQ(out.mapping.moveSpeed, 7.0f);
}

// ---- The history -------------------------------------------------------------------------------------------------------

TEST(PredictionHistoryTest, RecordsRisingTicksOnly)
{
    PredictionHistory history;
    EXPECT_TRUE(history.Empty());
    EXPECT_TRUE(history.Record(10, Move(1.0f), kDt, At(1.0f)));
    EXPECT_FALSE(history.Record(10, Move(1.0f), kDt, At(2.0f))) << "the same tick twice";
    EXPECT_FALSE(history.Record(9, Move(1.0f), kDt, At(2.0f))) << "a tick before the newest";
    EXPECT_TRUE(history.Record(12, Move(1.0f), kDt, At(3.0f))) << "a gap is fine";
    EXPECT_EQ(history.Size(), 2u);
    std::uint32_t tick = 0;
    ASSERT_TRUE(history.NewestTick(tick));
    EXPECT_EQ(tick, 12u);
    EXPECT_EQ(history.NewestState()->position.x, 3.0f);
    ASSERT_NE(history.StateAt(10), nullptr);
    EXPECT_EQ(history.StateAt(10)->position.x, 1.0f);
    EXPECT_EQ(history.StateAt(11), nullptr);
}

TEST(PredictionHistoryTest, TheOldestSampleLeavesWhenTheHistoryIsFull)
{
    PredictionHistory history(8);
    EXPECT_EQ(history.Capacity(), 8u);
    for (std::uint32_t i = 0; i < 20; ++i)
    {
        EXPECT_TRUE(history.Record(100 + i, Move(1.0f), kDt, At(static_cast<float>(i))));
    }
    EXPECT_EQ(history.Size(), 8u);
    EXPECT_EQ(history.StateAt(111), nullptr);
    ASSERT_NE(history.StateAt(112), nullptr);
    EXPECT_EQ(history.StateAt(112)->position.x, 12.0f);
    ASSERT_NE(history.StateAt(119), nullptr);
}

TEST(PredictionHistoryTest, ATickBeforeTheWrapIsBeforeATickAfterIt)
{
    PredictionHistory history;
    const std::uint32_t last = 0xFFFFFFFFu;
    EXPECT_TRUE(history.Record(last - 1, Move(1.0f), kDt, At(1.0f)));
    EXPECT_TRUE(history.Record(last, Move(1.0f), kDt, At(2.0f)));
    EXPECT_TRUE(history.Record(0, Move(1.0f), kDt, At(3.0f)));
    EXPECT_TRUE(history.Record(1, Move(1.0f), kDt, At(4.0f)));
    EXPECT_FALSE(history.Record(last, Move(1.0f), kDt, At(5.0f)));
    EXPECT_EQ(history.Size(), 4u);

    // The server's state after tick `last` (x = 2) matches; the later ticks stay
    const ReconcileResult result = history.Reconcile(last, At(2.0f), kStepper, Options());
    EXPECT_EQ(result.outcome, ReconcileOutcome::Match);
    EXPECT_EQ(history.Size(), 2u);
    EXPECT_EQ(history.StateAt(0)->position.x, 3.0f);
}

TEST(PredictionHistoryTest, AgreeingStateConfirmsAndDropsTheHistoryUpToIt)
{
    PredictionHistory history = Recorded(100, 10, Move(6.0f));
    const CharacterState predicted = *history.StateAt(103);
    CharacterState server = predicted;
    server.position.x += 0.004f; // inside the millimetre quantisation plus the epsilon
    const ReconcileResult result = history.Reconcile(103, server, kStepper, Options());
    EXPECT_EQ(result.outcome, ReconcileOutcome::Match);
    EXPECT_TRUE(result.compared);
    EXPECT_EQ(result.replayed, 0u);
    EXPECT_NEAR(result.error, 0.004f, 1e-5f);
    EXPECT_EQ(result.state.position.x, predicted.position.x) << "a match keeps the client's own state";
    EXPECT_EQ(history.Size(), 6u);
    EXPECT_EQ(history.StateAt(103), nullptr);
    EXPECT_NE(history.StateAt(104), nullptr);
}

TEST(PredictionHistoryTest, ADifferenceSetsTheServerStateAndReplaysTheLaterInputs)
{
    PredictionHistory history;
    // Input x speeds 6, 12, 6, 12, 6: steps of 0.1, 0.2, 0.1, 0.2, 0.1
    const float speeds[5] = {6.0f, 12.0f, 6.0f, 12.0f, 6.0f};
    CharacterState state = At(0.0f);
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        state = Scripted(state, Move(speeds[i]), kDt);
        ASSERT_TRUE(history.Record(50 + i, Move(speeds[i]), kDt, state));
    }
    // The server pushed the character: after tick 51 it is at x = 10, not 0.3
    const ReconcileResult result = history.Reconcile(51, At(10.0f), kStepper, Options());
    EXPECT_EQ(result.outcome, ReconcileOutcome::Corrected);
    EXPECT_TRUE(result.compared);
    EXPECT_EQ(result.replayed, 3u);
    EXPECT_NEAR(result.error, 10.0f - 0.3f, 1e-4f);
    // 10 + 0.1 + 0.2 + 0.1 = ticks 52, 53, 54 on the server's state
    EXPECT_NEAR(result.state.position.x, 10.4f, 1e-4f);
    // The history now holds the replayed states, from the first unacknowledged tick
    EXPECT_EQ(history.Size(), 3u);
    EXPECT_NEAR(history.StateAt(52)->position.x, 10.1f, 1e-4f);
    EXPECT_NEAR(history.StateAt(53)->position.x, 10.3f, 1e-4f);
    EXPECT_NEAR(history.StateAt(54)->position.x, 10.4f, 1e-4f);

    // The next state from the server agrees with the replayed one: no second correction
    const ReconcileResult next = history.Reconcile(53, At(10.3f), kStepper, Options());
    EXPECT_EQ(next.outcome, ReconcileOutcome::Match);
    EXPECT_EQ(history.Size(), 1u);
}

TEST(PredictionHistoryTest, ReplayIsTheSameAsRunningTheStepFromTheServersState)
{
    // A thousand-fold check of the central claim: reconciling at tick k gives the state a straight run from the server's
    // state over the later inputs gives, bit for bit
    for (std::uint32_t k = 0; k < 40; ++k)
    {
        PredictionHistory history;
        CharacterState state = At(0.0f);
        std::vector<CharacterInput> inputs;
        for (std::uint32_t i = 0; i < 40; ++i)
        {
            inputs.push_back(Move(static_cast<float>((i * 7 + 3) % 11) - 5.0f, static_cast<float>(i % 5)));
            state = Scripted(state, inputs.back(), kDt);
            ASSERT_TRUE(history.Record(1000 + i, inputs.back(), kDt, state));
        }
        const CharacterState server = At(100.0f + static_cast<float>(k), 2.0f, -3.0f);
        CharacterState expected = server;
        expected.velocity = Math::Vector3(0.0f, 0.0f, 0.0f);
        for (std::uint32_t i = k + 1; i < 40; ++i)
        {
            expected = Scripted(expected, inputs[i], kDt);
        }
        const ReconcileResult result = history.Reconcile(1000 + k, server, kStepper, Options());
        ASSERT_EQ(result.outcome, ReconcileOutcome::Corrected) << "k=" << k;
        EXPECT_EQ(result.replayed, 39 - k);
        EXPECT_EQ(result.state.position.x, expected.position.x) << "k=" << k;
        EXPECT_EQ(result.state.position.y, expected.position.y) << "k=" << k;
        EXPECT_EQ(result.state.position.z, expected.position.z) << "k=" << k;
    }
}

TEST(PredictionHistoryTest, VerticalSpeedAndGroundedAreComparedToo)
{
    const PredictionHistory base = Recorded(10, 5, Move(0.0f));

    PredictionHistory a = base;
    CharacterState server = *a.StateAt(12);
    server.velocity.y = 4.0f; // a jump the client did not predict
    EXPECT_EQ(a.Reconcile(12, server, kStepper, Options()).outcome, ReconcileOutcome::Corrected);

    PredictionHistory b = base;
    server = *b.StateAt(12);
    server.grounded = true;
    EXPECT_EQ(b.Reconcile(12, server, kStepper, Options()).outcome, ReconcileOutcome::Corrected);

    PredictionHistory c = base;
    server = *c.StateAt(12);
    server.velocity.y += 0.04f; // inside velocityEpsilon (0.05)
    EXPECT_EQ(c.Reconcile(12, server, kStepper, Options()).outcome, ReconcileOutcome::Match);
}

TEST(PredictionHistoryTest, AStateThatCannotBeUsedIsIgnoredAndChangesNothing)
{
    PredictionHistory history = Recorded(100, 10, Move(6.0f));
    const std::size_t size = history.Size();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // Not finite
    CharacterState bad = At(nan);
    EXPECT_EQ(history.Reconcile(105, bad, kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    bad = At(0.0f);
    bad.velocity.y = std::numeric_limits<float>::infinity();
    EXPECT_EQ(history.Reconcile(105, bad, kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    // An input the client never made (after the newest)
    EXPECT_EQ(history.Reconcile(110, At(0.0f), kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    EXPECT_EQ(history.Reconcile(5000, At(0.0f), kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    EXPECT_EQ(history.Size(), size);

    // Nothing recorded
    PredictionHistory empty;
    EXPECT_EQ(empty.Reconcile(1, At(0.0f), kStepper, Options()).outcome, ReconcileOutcome::Ignored);
}

TEST(PredictionHistoryTest, ATickBetweenTwoSamplesIsIgnored)
{
    PredictionHistory history;
    EXPECT_TRUE(history.Record(10, Move(1.0f), kDt, At(1.0f)));
    EXPECT_TRUE(history.Record(14, Move(1.0f), kDt, At(2.0f)));
    EXPECT_TRUE(history.Record(15, Move(1.0f), kDt, At(3.0f)));
    EXPECT_EQ(history.Reconcile(12, At(9.0f), kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    EXPECT_EQ(history.Size(), 3u);
}

TEST(PredictionHistoryTest, AStateOlderThanWhatWasConfirmedIsIgnored)
{
    PredictionHistory history = Recorded(100, 10, Move(6.0f));
    EXPECT_EQ(history.Reconcile(105, *history.StateAt(105), kStepper, Options()).outcome, ReconcileOutcome::Match);
    // Reordered: the server's state for 102 arrives after the one for 105
    EXPECT_EQ(history.Reconcile(102, At(50.0f), kStepper, Options()).outcome, ReconcileOutcome::Ignored);
    EXPECT_EQ(history.Size(), 4u);
}

TEST(PredictionHistoryTest, TheSameTickTwiceReplaysWithoutComparing)
{
    // The server applied the one input on two ticks (a frame repeated), so two states name it
    PredictionHistory history = Recorded(100, 10, Move(6.0f));
    EXPECT_EQ(history.Reconcile(105, *history.StateAt(105), kStepper, Options()).outcome, ReconcileOutcome::Match);
    const ReconcileResult again = history.Reconcile(105, At(1.0f), kStepper, Options());
    EXPECT_EQ(again.outcome, ReconcileOutcome::Corrected);
    EXPECT_FALSE(again.compared);
    EXPECT_EQ(again.replayed, 4u) << "ticks 106 to 109 on the server's state";
    EXPECT_NEAR(again.state.position.x, 1.0f + 4.0f * 0.1f, 1e-4f);
}

TEST(PredictionHistoryTest, AStateOlderThanTheWindowSnapsAndDropsTheHistory)
{
    PredictionHistory history(8);
    CharacterState state = At(0.0f);
    for (std::uint32_t i = 0; i < 30; ++i)
    {
        state = Scripted(state, Move(6.0f), kDt);
        ASSERT_TRUE(history.Record(200 + i, Move(6.0f), kDt, state));
    }
    // Held: 222 to 229
    ASSERT_EQ(history.StateAt(221), nullptr);
    const ReconcileResult result = history.Reconcile(210, At(-5.0f, 1.0f, 2.0f), kStepper, Options());
    EXPECT_EQ(result.outcome, ReconcileOutcome::Overflow);
    EXPECT_EQ(result.replayed, 0u);
    EXPECT_EQ(result.state.position.x, -5.0f);
    EXPECT_EQ(result.state.position.y, 1.0f);
    EXPECT_GT(result.error, 1.0f);
    EXPECT_TRUE(history.Empty());

    // The ticks of the dropped history still count as overflowed
    EXPECT_TRUE(history.Record(230, Move(6.0f), kDt, At(0.0f)));
    EXPECT_EQ(history.Reconcile(228, At(0.0f), kStepper, Options()).outcome, ReconcileOutcome::Overflow);
}

TEST(PredictionHistoryTest, ClearForgetsEverything)
{
    PredictionHistory history = Recorded(100, 10, Move(6.0f));
    EXPECT_EQ(history.Reconcile(103, *history.StateAt(103), kStepper, Options()).outcome, ReconcileOutcome::Match);
    history.Clear();
    EXPECT_TRUE(history.Empty());
    EXPECT_TRUE(history.Record(50, Move(1.0f), kDt, At(1.0f))) << "a smaller tick records again after a clear";
    // The confirmed mark went with it: an older tick is not "older than confirmed"
    EXPECT_EQ(history.Reconcile(50, At(1.0f), kStepper, Options()).outcome, ReconcileOutcome::Match);
}

TEST(PredictionHistoryTest, TheReplayIsBoundedByTheCapacity)
{
    PredictionHistory history(8);
    CharacterState state = At(0.0f);
    int calls = 0;
    for (std::uint32_t i = 0; i < 500; ++i)
    {
        state = Scripted(state, Move(6.0f), kDt);
        ASSERT_TRUE(history.Record(i, Move(6.0f), kDt, state));
    }
    const PredictionStepper counting = [&](const CharacterState &s, const CharacterInput &in, const float dt)
    {
        ++calls;
        return Scripted(s, in, dt);
    };
    const ReconcileResult result = history.Reconcile(492, At(99.0f), counting, Options());
    EXPECT_EQ(result.outcome, ReconcileOutcome::Corrected);
    EXPECT_EQ(calls, 7);
    EXPECT_LE(static_cast<std::size_t>(calls), history.Capacity());
}
