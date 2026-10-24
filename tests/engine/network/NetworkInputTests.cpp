#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkInput.hpp"

// The pieces of player input that need no scene and no connection (netcode stage B4): the layout and its quantisation,
// the InputFrames and InputAck messages, the server's jitter buffer and the client's ring. The end-to-end behaviour on
// the network simulator is in NetworkInputSessionTests.cpp.

using namespace N2Engine::Network;

namespace
{
    InputControlSpec Button(const std::string &name)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = InputControlKind::Button;
        return spec;
    }

    InputControlSpec Axis(const std::string &name, const std::uint8_t bits, const float range, const bool isSigned)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = InputControlKind::Axis;
        spec.bits = bits;
        spec.range = range;
        spec.isSigned = isSigned;
        return spec;
    }

    InputControlSpec Vec2(const std::string &name, const std::uint8_t bits, const float range)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = InputControlKind::Vector2;
        spec.bits = bits;
        spec.range = range;
        spec.isSigned = true;
        return spec;
    }

    /// jump (1 bit), move (2 x 8 bits), throttle (unsigned 6 bits): 23 bits, 3 bytes. Sorted: jump, move, throttle.
    InputLayout GameLayout()
    {
        auto layout = InputLayout::Create({Axis("throttle", 6, 1.0f, false), Vec2("move", 8, 1.0f), Button("jump")});
        EXPECT_TRUE(layout.has_value());
        return layout.has_value() ? std::move(*layout) : InputLayout();
    }

    std::vector<float> Values(const float jump, const float moveX, const float moveY, const float throttle)
    {
        return {jump, moveX, moveY, throttle};
    }

    InputFrame Frame(const InputLayout &layout, const std::uint32_t tick, const std::uint32_t sequence,
                     const float throttle = 0.0f, const float jump = 0.0f)
    {
        InputFrame frame;
        frame.tick = tick;
        frame.sequence = sequence;
        frame.payload = layout.Pack(Values(jump, 0.0f, 0.0f, throttle)).value();
        return frame;
    }

    /// The throttle value (index 3) a resolved payload holds
    float ThrottleOf(const InputLayout &layout, const ResolvedInput &input)
    {
        return layout.Unpack(input.payload).value()[3];
    }
}

// ---- The layout ------------------------------------------------------------------------------------------------------

TEST(InputLayoutTest, ControlsAreSortedByNameAndOffsetsFollow)
{
    const InputLayout layout = GameLayout();
    ASSERT_EQ(layout.ControlCount(), 3u);
    EXPECT_EQ(layout.Control(0).name, "jump");
    EXPECT_EQ(layout.Control(1).name, "move");
    EXPECT_EQ(layout.Control(2).name, "throttle");
    EXPECT_EQ(layout.ValueCount(), 4u);
    EXPECT_EQ(layout.ValueOffset(0), 0u);
    EXPECT_EQ(layout.ValueOffset(1), 1u);
    EXPECT_EQ(layout.ValueOffset(2), 3u);
    EXPECT_EQ(layout.ComponentsOf(1), 2u);
    EXPECT_EQ(layout.BitCount(), 1u + 16u + 6u);
    EXPECT_EQ(layout.PayloadBytes(), 3u);
    ASSERT_TRUE(layout.FindControl("move").has_value());
    EXPECT_EQ(*layout.FindControl("move"), 1u);
    EXPECT_FALSE(layout.FindControl("missing").has_value());
}

TEST(InputLayoutTest, TheDeclarationOrderDoesNotChangeTheLayoutOrItsHash)
{
    const auto a = InputLayout::Create({Button("a"), Button("b"), Axis("c", 8, 1.0f, true)});
    const auto b = InputLayout::Create({Axis("c", 8, 1.0f, true), Button("b"), Button("a")});
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->Hash(), b->Hash());
}

TEST(InputLayoutTest, EveryPartOfAControlChangesTheHash)
{
    const std::uint64_t base = InputLayout::Create({Axis("x", 8, 1.0f, true)})->Hash();
    EXPECT_NE(base, InputLayout::Create({Axis("y", 8, 1.0f, true)})->Hash());
    EXPECT_NE(base, InputLayout::Create({Axis("x", 9, 1.0f, true)})->Hash());
    EXPECT_NE(base, InputLayout::Create({Axis("x", 8, 2.0f, true)})->Hash());
    EXPECT_NE(base, InputLayout::Create({Axis("x", 8, 1.0f, false)})->Hash());
    EXPECT_NE(base, InputLayout::Create({Vec2("x", 8, 1.0f)})->Hash());
    EXPECT_NE(base, InputLayout().Hash());
    EXPECT_EQ(base, InputLayout::Create({Axis("x", 8, 1.0f, true)})->Hash());
}

TEST(InputLayoutTest, TheLayoutIsPartOfTheRegistryHashOfTheFingerprint)
{
    const PrefabTable prefabs;
    const InputLayout layout = GameLayout();
    const InputLayout other = InputLayout::Create({Button("jump")}).value();
    EXPECT_NE(HashRegistry(prefabs, layout), HashRegistry(prefabs, other));
    EXPECT_NE(HashRegistry(prefabs, layout), HashRegistry(prefabs));
    EXPECT_EQ(HashRegistry(prefabs, layout), HashRegistry(prefabs, GameLayout()));

    const Fingerprint a = MakeFingerprint("res://a.scene", 7, 0, prefabs, layout);
    const Fingerprint b = MakeFingerprint("res://a.scene", 7, 0, prefabs, other);
    EXPECT_NE(a.registryHash, b.registryHash);
    EXPECT_EQ(CompareFingerprints(a, b), JoinRefusal::Registry);
    EXPECT_EQ(CompareFingerprints(a, MakeFingerprint("res://a.scene", 7, 0, prefabs, GameLayout())), JoinRefusal::None);
}

TEST(InputLayoutTest, CreateRejectsEverythingOutOfBounds)
{
    EXPECT_FALSE(InputLayout::Create({Button("")}).has_value());
    EXPECT_FALSE(InputLayout::Create({Button("1abc")}).has_value());
    EXPECT_FALSE(InputLayout::Create({Button("has space")}).has_value());
    EXPECT_FALSE(InputLayout::Create({Button(std::string(kMaxInputControlNameLength + 1, 'a'))}).has_value());
    EXPECT_TRUE(InputLayout::Create({Button(std::string(kMaxInputControlNameLength, 'a'))}).has_value());
    EXPECT_TRUE(InputLayout::Create({Button("_ok_1")}).has_value());
    EXPECT_FALSE(InputLayout::Create({Button("a"), Button("a")}).has_value());

    EXPECT_FALSE(InputLayout::Create({Axis("x", 0, 1.0f, false)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 1, 1.0f, true)}).has_value());
    EXPECT_TRUE(InputLayout::Create({Axis("x", 1, 1.0f, false)}).has_value());
    EXPECT_TRUE(InputLayout::Create({Axis("x", 16, 1.0f, true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 17, 1.0f, true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 8, 0.0f, true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 8, -1.0f, true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 8, std::numeric_limits<float>::quiet_NaN(), true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 8, std::numeric_limits<float>::infinity(), true)}).has_value());
    EXPECT_FALSE(InputLayout::Create({Axis("x", 8, kMaxInputRange * 2.0f, true)}).has_value());

    InputControlSpec unknownKind = Button("k");
    unknownKind.kind = static_cast<InputControlKind>(9);
    EXPECT_FALSE(InputLayout::Create({unknownKind}).has_value());

    // More than the control cap
    std::vector<InputControlSpec> many;
    for (std::size_t i = 0; i <= kMaxInputControls; ++i)
    {
        many.push_back(Button("c" + std::to_string(i)));
    }
    EXPECT_FALSE(InputLayout::Create(many).has_value());
    many.pop_back();
    EXPECT_TRUE(InputLayout::Create(many).has_value());

    // More bits than a payload holds: 11 Vector3 of 8 bits = 264 bits
    std::vector<InputControlSpec> wide;
    for (int i = 0; i < 11; ++i)
    {
        InputControlSpec spec = Vec2("v" + std::to_string(i), 8, 1.0f);
        spec.kind = InputControlKind::Vector3;
        wide.push_back(spec);
    }
    EXPECT_FALSE(InputLayout::Create(wide).has_value());
    wide.pop_back();
    EXPECT_TRUE(InputLayout::Create(wide).has_value());
}

TEST(InputLayoutTest, AButtonIgnoresTheFieldsThatDoNotApplyToIt)
{
    InputControlSpec odd = Button("b");
    odd.bits = 99;
    odd.range = -5.0f;
    const auto layout = InputLayout::Create({odd});
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->Hash(), InputLayout::Create({Button("b")})->Hash());
    EXPECT_EQ(layout->BitCount(), 1u);
}

TEST(InputLayoutTest, ParseKindMatchesToString)
{
    for (const InputControlKind kind : {InputControlKind::Button, InputControlKind::Axis, InputControlKind::Vector2,
                                        InputControlKind::Vector3})
    {
        const auto parsed = ParseInputControlKind(ToString(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
    EXPECT_FALSE(ParseInputControlKind("Button").has_value());
    EXPECT_FALSE(ParseInputControlKind("").has_value());
}

TEST(InputLayoutTest, QuantisationRoundTripsToTheNearestStep)
{
    const InputLayout layout = GameLayout();
    // 8 signed bits: 127 steps each side of zero, a step of 1/127; throttle: 63 steps of 1/63
    const float step8 = 1.0f / 127.0f;
    const float step6 = 1.0f / 63.0f;
    for (int i = -200; i <= 200; ++i)
    {
        const float x = static_cast<float>(i) / 200.0f;
        const float y = -x * 0.37f;
        const float t = (x + 1.0f) / 2.0f;
        const auto packed = layout.Pack(Values(0.0f, x, y, t));
        ASSERT_TRUE(packed.has_value());
        ASSERT_EQ(packed->size(), layout.PayloadBytes());
        const auto back = layout.Unpack(*packed);
        ASSERT_TRUE(back.has_value());
        EXPECT_LE(std::fabs((*back)[1] - x), step8 * 0.5f + 1e-6f) << x;
        EXPECT_LE(std::fabs((*back)[2] - y), step8 * 0.5f + 1e-6f) << y;
        EXPECT_LE(std::fabs((*back)[3] - t), step6 * 0.5f + 1e-6f) << t;
        // Quantising a decoded value changes nothing: the wire value is a fixed point
        const auto again = layout.Pack(*back);
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(*again, *packed);
    }
}

TEST(InputLayoutTest, ZeroAndTheEndsOfTheRangeAreExact)
{
    const InputLayout layout = GameLayout();
    const auto zero = layout.Unpack(layout.Pack(Values(0.0f, 0.0f, 0.0f, 0.0f)).value());
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ((*zero)[1], 0.0f);
    EXPECT_EQ((*zero)[2], 0.0f);
    EXPECT_EQ((*zero)[3], 0.0f);

    const auto ends = layout.Unpack(layout.Pack(Values(1.0f, 1.0f, -1.0f, 1.0f)).value());
    ASSERT_TRUE(ends.has_value());
    EXPECT_EQ((*ends)[0], 1.0f);
    EXPECT_EQ((*ends)[1], 1.0f);
    EXPECT_EQ((*ends)[2], -1.0f);
    EXPECT_EQ((*ends)[3], 1.0f);

    // The neutral payload is the all-zero input
    const auto neutral = layout.Unpack(layout.NeutralPayload());
    ASSERT_TRUE(neutral.has_value());
    EXPECT_EQ(*neutral, layout.NeutralValues());
}

TEST(InputLayoutTest, OutOfRangeAndNonFiniteValuesClampAndNaNIsZero)
{
    const InputLayout layout = GameLayout();
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto back = layout.Unpack(layout.Pack(Values(nan, 5.0f, -inf, -3.0f)).value());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ((*back)[0], 0.0f);  // NaN is not a pressed button
    EXPECT_EQ((*back)[1], 1.0f);  // clamped to the range
    EXPECT_EQ((*back)[2], -1.0f); // minus infinity is the low end
    EXPECT_EQ((*back)[3], 0.0f);  // an unsigned axis stops at zero

    const auto nanAxis = layout.Unpack(layout.Pack(Values(0.0f, nan, nan, nan)).value());
    ASSERT_TRUE(nanAxis.has_value());
    EXPECT_EQ((*nanAxis)[1], 0.0f);
    EXPECT_EQ((*nanAxis)[3], 0.0f);

    // A button is pressed from 0.5 up
    EXPECT_EQ(layout.Unpack(layout.Pack(Values(0.49f, 0, 0, 0)).value()).value()[0], 0.0f);
    EXPECT_EQ(layout.Unpack(layout.Pack(Values(0.5f, 0, 0, 0)).value()).value()[0], 1.0f);
}

TEST(InputLayoutTest, PackIsDeterministicAndHasAFixedBitOrder)
{
    const InputLayout layout = GameLayout();
    const auto a = layout.Pack(Values(1.0f, 0.5f, -0.25f, 0.75f)).value();
    const auto b = layout.Pack(Values(1.0f, 0.5f, -0.25f, 0.75f)).value();
    EXPECT_EQ(a, b);

    // jump = 1 (bit 0); move.x: 0.5 -> step 64 (63.5 rounds up) + 127 = 191 (bits 1..8); move.y: -0.25 -> -32 + 127 = 95
    // (bits 9..16); throttle: 0.75 * 63 = 47.25 -> 47 (bits 17..22)
    const std::uint32_t expected = 1u | (191u << 1) | (95u << 9) | (47u << 17);
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[0], static_cast<std::uint8_t>(expected & 0xFF));
    EXPECT_EQ(a[1], static_cast<std::uint8_t>((expected >> 8) & 0xFF));
    EXPECT_EQ(a[2], static_cast<std::uint8_t>((expected >> 16) & 0xFF));
}

TEST(InputLayoutTest, PackRefusesTheWrongNumberOfValues)
{
    const InputLayout layout = GameLayout();
    EXPECT_FALSE(layout.Pack(std::vector<float>{1.0f}).has_value());
    EXPECT_FALSE(layout.Pack(std::vector<float>(5, 0.0f)).has_value());
}

TEST(InputLayoutTest, OnlyThePayloadsAPackProducesAreValid)
{
    const InputLayout layout = GameLayout();
    std::vector<std::uint8_t> payload = layout.Pack(Values(1.0f, 0.5f, 0.5f, 0.5f)).value();
    EXPECT_TRUE(layout.IsValidPayload(payload));

    // The wrong length
    EXPECT_FALSE(layout.IsValidPayload(std::vector<std::uint8_t>(2, 0)));
    EXPECT_FALSE(layout.IsValidPayload(std::vector<std::uint8_t>(4, 0)));
    EXPECT_FALSE(layout.IsValidPayload({}));

    // A padding bit set (bit 23 of 24)
    std::vector<std::uint8_t> padded = payload;
    padded[2] = static_cast<std::uint8_t>(padded[2] | 0x80);
    EXPECT_FALSE(layout.IsValidPayload(padded));

    // A signed 8-bit component at 255: past the largest step (254)
    std::vector<std::uint8_t> high(3, 0);
    high[0] = 0xFE; // jump 0, move.x bits 1..7 all set
    high[1] = 0x01; // move.x bit 8 set: 255
    EXPECT_FALSE(layout.IsValidPayload(high));
    EXPECT_FALSE(layout.Unpack(high).has_value());
}

TEST(InputLayoutTest, TheEmptyLayoutHasNoPayload)
{
    const InputLayout empty;
    EXPECT_TRUE(empty.Empty());
    EXPECT_EQ(empty.PayloadBytes(), 0u);
    EXPECT_EQ(empty.ValueCount(), 0u);
    EXPECT_TRUE(empty.IsValidPayload({}));
}

TEST(InputLayoutTest, TheDeclaredLayoutIsTheLastOneSet)
{
    SetDeclaredInputLayout(GameLayout());
    EXPECT_EQ(DeclaredInputLayout().Hash(), GameLayout().Hash());
    SetDeclaredInputLayout(InputLayout());
    EXPECT_TRUE(DeclaredInputLayout().Empty());
}

// ---- The messages ----------------------------------------------------------------------------------------------------

TEST(InputMessageTest, FramesRoundTrip)
{
    const InputLayout layout = GameLayout();
    std::vector<InputFrame> frames = {Frame(layout, 10, 1, 0.1f), Frame(layout, 11, 2, 0.2f, 1.0f),
                                      Frame(layout, 12, 3, 0.3f)};
    const std::vector<std::uint8_t> bytes = EncodeInputFrames(frames, layout.PayloadBytes());
    ASSERT_EQ(bytes.size(), 2u + 3u * (kInputFrameHeaderBytes + layout.PayloadBytes()));
    EXPECT_EQ(bytes[0], kInputFramesType);
    EXPECT_EQ(bytes[1], 3);

    const auto decoded = DecodeInputFrames(bytes, layout.PayloadBytes());
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 3u);
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ((*decoded)[i].tick, frames[i].tick);
        EXPECT_EQ((*decoded)[i].sequence, frames[i].sequence);
        EXPECT_EQ((*decoded)[i].payload, frames[i].payload);
    }
    // The same frames always encode to the same bytes
    EXPECT_EQ(EncodeInputFrames(frames, layout.PayloadBytes()), bytes);
}

TEST(InputMessageTest, TheLargestMessageIsBounded)
{
    EXPECT_EQ(kMaxInputMessageBytes, 2u + 8u * (8u + 32u));
    std::vector<InputFrame> frames;
    for (std::uint32_t i = 0; i < kMaxInputFramesPerMessage; ++i)
    {
        InputFrame frame;
        frame.tick = 100 + i;
        frame.sequence = 1 + i;
        frame.payload.assign(kMaxInputPayloadBytes, 0);
        frames.push_back(frame);
    }
    const auto bytes = EncodeInputFrames(frames, kMaxInputPayloadBytes);
    EXPECT_EQ(bytes.size(), kMaxInputMessageBytes);
    EXPECT_TRUE(DecodeInputFrames(bytes, kMaxInputPayloadBytes).has_value());

    // One more frame than a message holds does not encode
    frames.push_back(frames.back());
    EXPECT_TRUE(EncodeInputFrames(frames, kMaxInputPayloadBytes).empty());
    EXPECT_TRUE(EncodeInputFrames({}, 3).empty());
    // A payload of the wrong size does not encode
    std::vector<InputFrame> wrong = {Frame(GameLayout(), 1, 1)};
    EXPECT_TRUE(EncodeInputFrames(wrong, 4).empty());
    EXPECT_TRUE(EncodeInputFrames(wrong, kMaxInputPayloadBytes + 1).empty());
}

TEST(InputMessageTest, DecodeRefusesEveryMalformedMessage)
{
    const InputLayout layout = GameLayout();
    const std::size_t payload = layout.PayloadBytes();
    const std::vector<InputFrame> two = {Frame(layout, 10, 1), Frame(layout, 11, 2)};
    const std::vector<std::uint8_t> good = EncodeInputFrames(two, payload);

    EXPECT_EQ(DecodeInputFrames({}, payload).error(), DecodeError::Empty);

    std::vector<std::uint8_t> wrongType = good;
    wrongType[0] = kStateUpdateType;
    EXPECT_EQ(DecodeInputFrames(wrongType, payload).error(), DecodeError::UnknownType);

    std::vector<std::uint8_t> zero = good;
    zero[1] = 0;
    EXPECT_EQ(DecodeInputFrames(zero, payload).error(), DecodeError::BadValue);
    std::vector<std::uint8_t> nine = good;
    nine[1] = 9;
    EXPECT_EQ(DecodeInputFrames(nine, payload).error(), DecodeError::BadValue);
    std::vector<std::uint8_t> huge = good;
    huge[1] = 255;
    EXPECT_EQ(DecodeInputFrames(huge, payload).error(), DecodeError::BadValue);

    // A count that claims more than the bytes hold, and the reverse
    std::vector<std::uint8_t> claimsMore = good;
    claimsMore[1] = 3;
    EXPECT_EQ(DecodeInputFrames(claimsMore, payload).error(), DecodeError::Truncated);
    std::vector<std::uint8_t> claimsLess = good;
    claimsLess[1] = 1;
    EXPECT_EQ(DecodeInputFrames(claimsLess, payload).error(), DecodeError::TrailingBytes);

    std::vector<std::uint8_t> shortened(good.begin(), good.end() - 1);
    EXPECT_EQ(DecodeInputFrames(shortened, payload).error(), DecodeError::Truncated);
    std::vector<std::uint8_t> longer = good;
    longer.push_back(0);
    EXPECT_EQ(DecodeInputFrames(longer, payload).error(), DecodeError::TrailingBytes);
    EXPECT_EQ(DecodeInputFrames(std::vector<std::uint8_t>{kInputFramesType}, payload).error(), DecodeError::Truncated);

    // A layout whose payload is larger than any layout may have
    EXPECT_EQ(DecodeInputFrames(good, kMaxInputPayloadBytes + 1).error(), DecodeError::BadValue);

    // Sequence 0, and ticks or sequences that do not rise
    EXPECT_EQ(DecodeInputFrames(EncodeInputFrames(std::vector<InputFrame>{Frame(layout, 5, 0)}, payload), payload).error(),
              DecodeError::BadValue);
    EXPECT_EQ(DecodeInputFrames(EncodeInputFrames(std::vector<InputFrame>{Frame(layout, 5, 1), Frame(layout, 5, 2)}, payload),
                                payload)
                  .error(),
              DecodeError::BadValue);
    EXPECT_EQ(DecodeInputFrames(EncodeInputFrames(std::vector<InputFrame>{Frame(layout, 5, 3), Frame(layout, 6, 3)}, payload),
                                payload)
                  .error(),
              DecodeError::BadValue);
    EXPECT_EQ(DecodeInputFrames(EncodeInputFrames(std::vector<InputFrame>{Frame(layout, 6, 1), Frame(layout, 5, 2)}, payload),
                                payload)
                  .error(),
              DecodeError::BadValue);
}

TEST(InputMessageTest, TicksAndSequencesMayWrapAround)
{
    const InputLayout layout = GameLayout();
    const std::vector<InputFrame> frames = {Frame(layout, 0xFFFFFFFEu, 0xFFFFFFFEu), Frame(layout, 0xFFFFFFFFu, 0xFFFFFFFFu),
                                            Frame(layout, 0u, 1u), Frame(layout, 1u, 2u)};
    const auto bytes = EncodeInputFrames(frames, layout.PayloadBytes());
    const auto decoded = DecodeInputFrames(bytes, layout.PayloadBytes());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 4u);
}

TEST(InputMessageTest, TheAckRoundTripsAndIsExact)
{
    const std::vector<std::uint8_t> bytes = Encode(InputAckMessage{77, 0xDEADBEEFu});
    ASSERT_EQ(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], kInputAckType);
    const auto decoded = DecodeInputAck(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->latest, 77u);
    EXPECT_EQ(decoded->bits, 0xDEADBEEFu);

    EXPECT_EQ(DecodeInputAck({}).error(), DecodeError::Empty);
    EXPECT_EQ(DecodeInputAck(std::vector<std::uint8_t>{kStateAckType, 1, 0, 0, 0, 0, 0, 0, 0}).error(), DecodeError::UnknownType);
    EXPECT_EQ(DecodeInputAck(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 1)).error(), DecodeError::Truncated);
    std::vector<std::uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_EQ(DecodeInputAck(longer).error(), DecodeError::TrailingBytes);
    EXPECT_EQ(DecodeInputAck(Encode(InputAckMessage{0, 5})).error(), DecodeError::BadValue);
}

TEST(InputMessageTest, TheTypeBytesDoNotCollideWithTheStateMessages)
{
    EXPECT_NE(kInputFramesType, kStateAckType);
    EXPECT_NE(kInputFramesType, kStateUpdateType);
    EXPECT_NE(kInputAckType, kStateAckType);
    EXPECT_NE(kInputAckType, kStateUpdateType);
    EXPECT_GE(kInputFramesType, 0x81); // client to server
    EXPECT_LT(kInputAckType, 0x80);    // server to client
}

TEST(InputTickTest, ComparisonsAreModuloTwoToTheThirtyTwo)
{
    EXPECT_TRUE(TickBefore(5, 6));
    EXPECT_FALSE(TickBefore(6, 5));
    EXPECT_FALSE(TickBefore(5, 5));
    EXPECT_TRUE(TickBefore(0xFFFFFFFFu, 0));
    EXPECT_TRUE(TickBefore(0xFFFFFFF0u, 3));
    EXPECT_EQ(TickDifference(3, 0xFFFFFFF0u), 19);
    EXPECT_EQ(TickDifference(0xFFFFFFF0u, 3), -19);
}

// ---- The server's buffer ---------------------------------------------------------------------------------------------

namespace
{
    ServerInputOptions Options(const std::uint32_t delay, const std::uint32_t repeat = 3, const std::uint32_t future = 32)
    {
        ServerInputOptions options;
        options.delayTicks = delay;
        options.repeatLastTicks = repeat;
        options.maxFutureTicks = future;
        return options;
    }
}

TEST(ServerInputBufferTest, FramesAreAppliedOnTheirTickInOrderWhateverTheArrivalOrder)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0), 100);
    // Arrive out of order: 103, 101, 102 (sequences follow the ticks)
    EXPECT_EQ(buffer.Receive(Frame(layout, 103, 3, 0.75f)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Receive(Frame(layout, 101, 1, 0.25f)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Receive(Frame(layout, 102, 2, 0.5f)), InputReceive::Accepted);
    EXPECT_EQ(buffer.BufferedFrames(), 3u);

    const float step = 1.0f / 63.0f;
    for (std::uint32_t tick = 101; tick <= 103; ++tick)
    {
        const auto input = buffer.Resolve(tick);
        ASSERT_TRUE(input.has_value());
        EXPECT_EQ(input->tick, tick);
        EXPECT_EQ(input->source, tick == 100 ? InputSource::Neutral : InputSource::Frame);
        EXPECT_NEAR(ThrottleOf(layout, *input), 0.25f * static_cast<float>(tick - 100), step);
    }
    EXPECT_EQ(buffer.BufferedFrames(), 0u);
    EXPECT_EQ(buffer.Stats().ticksFromFrames, 3u);
}

TEST(ServerInputBufferTest, TheInputDelayShiftsTheApplyTick)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(2), 100);
    EXPECT_EQ(buffer.Receive(Frame(layout, 100, 1, 1.0f)), InputReceive::Accepted);
    // Stamped 100, applied on 102
    EXPECT_EQ(buffer.Resolve(101)->source, InputSource::Neutral);
    const auto at102 = buffer.Resolve(102);
    ASSERT_TRUE(at102.has_value());
    EXPECT_EQ(at102->source, InputSource::Frame);
    EXPECT_NEAR(ThrottleOf(layout, *at102), 1.0f, 1e-6f);
    // The delay is clamped to its cap
    ServerInputOptions huge;
    huge.delayTicks = 1000;
    EXPECT_EQ(ServerInputBuffer(layout, huge, 0).Options().delayTicks, kMaxInputDelayTicks);
}

TEST(ServerInputBufferTest, DuplicatesLateAndTooFarFramesAreRefusedAndCounted)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3, 10), 100);
    buffer.SetNow(100);
    EXPECT_EQ(buffer.Receive(Frame(layout, 100, 1)), InputReceive::Accepted);
    // The same frame again (redundancy), and another frame for a tick that is held
    EXPECT_EQ(buffer.Receive(Frame(layout, 100, 1)), InputReceive::Duplicate);
    EXPECT_EQ(buffer.Receive(Frame(layout, 100, 2)), InputReceive::Duplicate);
    EXPECT_EQ(buffer.BufferedFrames(), 1u);

    // Past the horizon: more than maxFutureTicks (10) ahead of now
    EXPECT_EQ(buffer.Receive(Frame(layout, 110, 3)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Receive(Frame(layout, 111, 4)), InputReceive::TooFar);
    EXPECT_EQ(buffer.Receive(Frame(layout, 5000, 5)), InputReceive::TooFar);

    // A tick already applied
    (void)buffer.Resolve(105);
    EXPECT_EQ(buffer.Receive(Frame(layout, 104, 6)), InputReceive::Late);
    EXPECT_EQ(buffer.Receive(Frame(layout, 105, 7)), InputReceive::Late);
    EXPECT_EQ(buffer.Receive(Frame(layout, 106, 8)), InputReceive::Accepted);

    const ServerInputStats &stats = buffer.Stats();
    EXPECT_EQ(stats.framesReceived, 9u);
    EXPECT_EQ(stats.framesAccepted, 3u);
    EXPECT_EQ(stats.framesDuplicate, 2u);
    EXPECT_EQ(stats.framesTooFar, 2u);
    EXPECT_EQ(stats.framesLate, 2u);
    EXPECT_EQ(stats.framesInvalid, 0u);
}

TEST(ServerInputBufferTest, ATooFarFrameIsNotAcknowledgedSoTheClientSendsItAgain)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3, 4), 100);
    buffer.SetNow(100);
    EXPECT_EQ(buffer.Receive(Frame(layout, 120, 1)), InputReceive::TooFar);
    EXPECT_FALSE(buffer.AckPending());
    buffer.SetNow(118);
    EXPECT_EQ(buffer.Receive(Frame(layout, 120, 1)), InputReceive::Accepted);
    EXPECT_TRUE(buffer.AckPending());
}

TEST(ServerInputBufferTest, APayloadTheLayoutDoesNotProduceIsInvalid)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0), 0);
    InputFrame frame = Frame(layout, 1, 1);
    frame.payload[2] = 0xFF; // padding bits
    EXPECT_EQ(buffer.Receive(frame), InputReceive::Invalid);
    InputFrame shortFrame = Frame(layout, 1, 2);
    shortFrame.payload.pop_back();
    EXPECT_EQ(buffer.Receive(shortFrame), InputReceive::Invalid);
    InputFrame noSequence = Frame(layout, 1, 3);
    noSequence.sequence = 0;
    EXPECT_EQ(buffer.Receive(noSequence), InputReceive::Invalid);
    EXPECT_EQ(buffer.BufferedFrames(), 0u);
    EXPECT_EQ(buffer.Stats().framesInvalid, 3u);
}

TEST(ServerInputBufferTest, AMissingTickRepeatsTheLastInputThenGoesNeutral)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3), 10);
    EXPECT_EQ(buffer.Receive(Frame(layout, 10, 1, 1.0f, 1.0f)), InputReceive::Accepted);

    const auto first = buffer.Resolve(10);
    EXPECT_EQ(first->source, InputSource::Frame);
    // Three missing ticks repeat the frame ...
    for (std::uint32_t tick = 11; tick <= 13; ++tick)
    {
        const auto repeated = buffer.Resolve(tick);
        ASSERT_TRUE(repeated.has_value());
        EXPECT_EQ(repeated->source, InputSource::Repeated) << tick;
        EXPECT_EQ(repeated->payload, first->payload) << tick;
    }
    // ... then neutral, which is the all-zero input
    for (std::uint32_t tick = 14; tick <= 18; ++tick)
    {
        const auto neutral = buffer.Resolve(tick);
        EXPECT_EQ(neutral->source, InputSource::Neutral) << tick;
        EXPECT_EQ(layout.Unpack(neutral->payload).value(), layout.NeutralValues()) << tick;
    }
    EXPECT_EQ(buffer.Stats().ticksRepeated, 3u);
    EXPECT_EQ(buffer.Stats().ticksNeutral, 5u);

    // A frame brings the real input back and the repeats are available again
    EXPECT_EQ(buffer.Receive(Frame(layout, 19, 2, 0.5f)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Resolve(19)->source, InputSource::Frame);
    EXPECT_EQ(buffer.Resolve(20)->source, InputSource::Repeated);
}

TEST(ServerInputBufferTest, WithNoFrameEverTheInputIsNeutral)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3), 0);
    EXPECT_EQ(buffer.Resolve(0)->source, InputSource::Neutral);
    EXPECT_EQ(buffer.Resolve(1)->source, InputSource::Neutral);
    EXPECT_EQ(buffer.Stats().ticksRepeated, 0u);
}

TEST(ServerInputBufferTest, RepeatingCanBeTurnedOff)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 0), 10);
    EXPECT_EQ(buffer.Receive(Frame(layout, 10, 1, 1.0f)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Resolve(10)->source, InputSource::Frame);
    EXPECT_EQ(buffer.Resolve(11)->source, InputSource::Neutral);
}

TEST(ServerInputBufferTest, ResolvingATickResolvesEveryEarlierTickInOrderAndAnswersAgain)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3), 50);
    EXPECT_EQ(buffer.Receive(Frame(layout, 51, 1, 0.5f)), InputReceive::Accepted);
    // Asking for 53 first: 50 is neutral, 51 the frame, 52 and 53 repeat it
    const auto at53 = buffer.Resolve(53);
    ASSERT_TRUE(at53.has_value());
    EXPECT_EQ(at53->source, InputSource::Repeated);
    EXPECT_EQ(buffer.NextTick(), 54u);
    EXPECT_EQ(buffer.Resolve(50)->source, InputSource::Neutral);
    EXPECT_EQ(buffer.Resolve(51)->source, InputSource::Frame);
    EXPECT_EQ(buffer.Resolve(52)->source, InputSource::Repeated);
    // The same answer however often it is asked
    EXPECT_EQ(buffer.Resolve(53)->payload, at53->payload);
    EXPECT_EQ(buffer.Stats().ticksFromFrames, 1u);
    EXPECT_EQ(buffer.Stats().ticksRepeated, 2u);
    EXPECT_EQ(buffer.Stats().ticksNeutral, 1u);
}

TEST(ServerInputBufferTest, OnlyTheLastTicksAreRemembered)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3), 0);
    (void)buffer.Resolve(200);
    EXPECT_TRUE(buffer.Resolve(200).has_value());
    EXPECT_TRUE(buffer.Resolve(200 - (kInputHistoryTicks - 1)).has_value());
    EXPECT_FALSE(buffer.Resolve(200 - kInputHistoryTicks).has_value());
    EXPECT_FALSE(buffer.Resolve(0).has_value());
    // The ticks nobody asked for were skipped, not stepped through
    EXPECT_GT(buffer.Stats().ticksSkipped, 0u);
}

TEST(ServerInputBufferTest, ABufferThatIsNeverReadDoesNotStallOrGrow)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3, 16), 0);
    std::uint32_t sequence = 1;
    for (std::uint32_t now = 0; now < 5000; ++now)
    {
        buffer.SetNow(now);
        EXPECT_NE(buffer.Receive(Frame(layout, now, sequence++)), InputReceive::TooFar);
        EXPECT_LE(buffer.BufferedFrames(), kInputWindowTicks);
    }
    // The cursor followed the clock, so a read now is current
    EXPECT_GE(TickDifference(buffer.NextTick(), 5000 - kInputHistoryTicks - 1), 0);
    EXPECT_EQ(buffer.Resolve(4999)->source, InputSource::Frame);
}

TEST(ServerInputBufferTest, AFloodOfFramesNeverHoldsMoreThanTheWindow)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0, 3, kMaxInputFutureTicks), 1000);
    buffer.SetNow(1000);
    std::uint32_t accepted = 0;
    // A million-tick spread of frames, ascending sequences
    for (std::uint32_t i = 1; i <= 20000; ++i)
    {
        const std::uint32_t tick = 1000 + i * 7919u;
        if (buffer.Receive(Frame(layout, tick, i)) == InputReceive::Accepted)
        {
            ++accepted;
        }
        EXPECT_LE(buffer.BufferedFrames(), kMaxInputFutureTicks + 1);
    }
    EXPECT_LE(accepted, kMaxInputFutureTicks + 1);
    // The same tick over and over holds one frame
    ServerInputBuffer same(layout, Options(0), 0);
    for (std::uint32_t i = 1; i <= 1000; ++i)
    {
        (void)same.Receive(Frame(layout, 5, i));
    }
    EXPECT_EQ(same.BufferedFrames(), 1u);
}

TEST(ServerInputBufferTest, SequencesAreAcknowledgedWithAWindowOfThirtyTwo)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0), 0);
    buffer.SetNow(0);
    EXPECT_FALSE(buffer.AckPending());
    (void)buffer.Receive(Frame(layout, 1, 1));
    (void)buffer.Receive(Frame(layout, 3, 3));
    ASSERT_TRUE(buffer.AckPending());
    InputAckMessage ack = buffer.TakeAck();
    EXPECT_EQ(ack.latest, 3u);
    EXPECT_EQ(ack.bits, 0b10u); // sequence 3 - 1 - 1 = 1 was received, 2 was not
    EXPECT_FALSE(buffer.AckPending());

    // A frame that arrives out of order is added to the bits
    (void)buffer.Receive(Frame(layout, 2, 2));
    ack = buffer.TakeAck();
    EXPECT_EQ(ack.latest, 3u);
    EXPECT_EQ(ack.bits, 0b11u);

    // A copy of an acknowledged frame changes nothing and asks for no new ack
    (void)buffer.Receive(Frame(layout, 3, 3));
    EXPECT_FALSE(buffer.AckPending());

    // A jump of more than 32 forgets the bits
    (void)buffer.Receive(Frame(layout, 4, 100));
    ack = buffer.TakeAck();
    EXPECT_EQ(ack.latest, 100u);
    EXPECT_EQ(ack.bits, 0u);
}

TEST(ServerInputBufferTest, TicksAndSequencesWrapAroundWithoutLosingAnything)
{
    const InputLayout layout = GameLayout();
    const std::uint32_t start = 0xFFFFFFF0u;
    ServerInputBuffer buffer(layout, Options(2, 3), start);
    std::uint32_t sequence = 0xFFFFFFF8u;
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        const std::uint32_t tick = start + i; // wraps to 0 after 16 steps
        buffer.SetNow(tick);
        EXPECT_EQ(buffer.Receive(Frame(layout, tick, sequence, static_cast<float>(i % 10) / 10.0f)), InputReceive::Accepted)
            << i;
        // A sender never uses 0: 0xFFFFFFFF is followed by 1
        ++sequence;
        if (sequence == 0)
        {
            sequence = 1;
        }
    }
    // Stamped t, applied on t + 2: tick start + 2 + i holds frame i
    for (std::uint32_t i = 0; i < 38; ++i)
    {
        const auto input = buffer.Resolve(start + 2 + i);
        ASSERT_TRUE(input.has_value());
        EXPECT_EQ(input->source, InputSource::Frame) << i;
        EXPECT_NEAR(ThrottleOf(layout, *input), static_cast<float>(i % 10) / 10.0f, 1.0f / 63.0f) << i;
    }
}

TEST(ServerInputBufferTest, AClockThatGoesBackStartsOver)
{
    const InputLayout layout = GameLayout();
    ServerInputBuffer buffer(layout, Options(0), 5000);
    (void)buffer.Resolve(5010);
    buffer.SetNow(10);
    EXPECT_EQ(buffer.NextTick(), 10u);
    EXPECT_EQ(buffer.Receive(Frame(layout, 12, 1)), InputReceive::Accepted);
    EXPECT_EQ(buffer.Resolve(12)->source, InputSource::Frame);
}

TEST(ServerInputBufferTest, TheSameFramesGiveTheSameInputOnEveryRun)
{
    const InputLayout layout = GameLayout();
    auto run = [&layout]
    {
        std::vector<std::vector<std::uint8_t>> out;
        std::vector<InputSource> sources;
        ServerInputBuffer buffer(layout, Options(2, 3), 100);
        // A fixed pattern: every third tick lost, pairs swapped
        std::vector<InputFrame> pending;
        for (std::uint32_t i = 0; i < 90; ++i)
        {
            buffer.SetNow(100 + i);
            const float t = static_cast<float>((i * 7) % 64) / 63.0f;
            if (i % 3 != 2)
            {
                pending.push_back(Frame(layout, 100 + i, i + 1, t, i % 2 == 0 ? 1.0f : 0.0f));
            }
            if (pending.size() == 2)
            {
                (void)buffer.Receive(pending[1]);
                (void)buffer.Receive(pending[0]);
                pending.clear();
            }
            const auto input = buffer.Resolve(100 + i);
            out.push_back(input->payload);
            sources.push_back(input->source);
        }
        return std::make_pair(out, sources);
    };
    const auto first = run();
    const auto second = run();
    EXPECT_EQ(first.first, second.first);
    EXPECT_EQ(first.second, second.second);
    // And the run held every kind of tick
    EXPECT_NE(std::count(first.second.begin(), first.second.end(), InputSource::Repeated), 0);
}

// ---- The client's ring -----------------------------------------------------------------------------------------------

TEST(ClientInputSenderTest, EachMessageCarriesTheNewestUnackedFrames)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 3);
    for (std::uint32_t tick = 1; tick <= 5; ++tick)
    {
        const auto sequence = sender.Queue(tick, Values(0, 0, 0, 0.1f * static_cast<float>(tick)));
        ASSERT_TRUE(sequence.has_value());
        EXPECT_EQ(*sequence, tick);
        EXPECT_TRUE(sender.HasPending());
        const auto message = sender.BuildMessage(1000);
        ASSERT_TRUE(message.has_value());
        EXPECT_FALSE(sender.HasPending());
        const auto frames = DecodeInputFrames(*message, layout.PayloadBytes());
        ASSERT_TRUE(frames.has_value());
        const std::size_t expected = tick < 3 ? tick : 3;
        ASSERT_EQ(frames->size(), expected);
        // Oldest first, the newest last
        EXPECT_EQ(frames->back().tick, tick);
        EXPECT_EQ(frames->back().sequence, tick);
        EXPECT_EQ(frames->front().sequence, tick - static_cast<std::uint32_t>(expected) + 1);
    }
    EXPECT_EQ(sender.Stats().messagesBuilt, 5u);
    EXPECT_EQ(sender.Stats().framesSent, 1u + 2u + 3u + 3u + 3u);
    EXPECT_EQ(sender.Unacked(), 5u);
}

TEST(ClientInputSenderTest, AcksTrimTheRingAndStopTheResend)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 4);
    for (std::uint32_t tick = 1; tick <= 4; ++tick)
    {
        ASSERT_TRUE(sender.Queue(tick, Values(0, 0, 0, 0)).has_value());
    }
    ASSERT_TRUE(sender.BuildMessage(1000).has_value());
    // Frames 4 (the latest), 2 and 1 acknowledged (bit i is sequence latest - 1 - i); 3 was lost
    EXPECT_TRUE(sender.OnAck(InputAckMessage{4, 0b110}));
    EXPECT_EQ(sender.Unacked(), 1u);
    ASSERT_TRUE(sender.Queue(5, Values(0, 0, 0, 0)).has_value());
    const auto frames = DecodeInputFrames(sender.BuildMessage(1000).value(), layout.PayloadBytes());
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 2u);
    EXPECT_EQ((*frames)[0].sequence, 3u); // the lost one goes again
    EXPECT_EQ((*frames)[1].sequence, 5u);
    EXPECT_EQ(sender.Stats().framesAcked, 3u);

    // An ack that arrives again changes nothing
    EXPECT_TRUE(sender.OnAck(InputAckMessage{4, 0b110}));
    EXPECT_EQ(sender.Unacked(), 2u);
    EXPECT_TRUE(sender.OnAck(InputAckMessage{5, 0b1111}));
    EXPECT_EQ(sender.Unacked(), 0u);
    // With nothing to send there is no message
    EXPECT_FALSE(sender.BuildMessage(1000).has_value());
}

TEST(ClientInputSenderTest, AnAckFarBehindTheNewestGivesUpOnOlderFrames)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 1);
    for (std::uint32_t tick = 1; tick <= 40; ++tick)
    {
        ASSERT_TRUE(sender.Queue(tick, Values(0, 0, 0, 0)).has_value());
        (void)sender.BuildMessage(1000);
    }
    ASSERT_EQ(sender.Unacked(), 40u);
    EXPECT_TRUE(sender.OnAck(InputAckMessage{40, 0}));
    // 40 is acknowledged; 7 and below are more than 32 behind and given up; 8 (exactly 32 behind, a bit the ack left
    // clear) to 39 wait
    EXPECT_EQ(sender.Unacked(), 32u);
    EXPECT_EQ(sender.Stats().framesAbandoned, 7u);
    EXPECT_EQ(sender.Stats().framesAcked, 1u);
}

TEST(ClientInputSenderTest, AnAckForASequenceNeverSentIsRefused)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 2);
    EXPECT_FALSE(sender.OnAck(InputAckMessage{1, 0})); // nothing sent yet
    ASSERT_TRUE(sender.Queue(1, Values(0, 0, 0, 0)).has_value());
    EXPECT_FALSE(sender.OnAck(InputAckMessage{1, 0})); // queued, not sent
    ASSERT_TRUE(sender.BuildMessage(1000).has_value());
    EXPECT_FALSE(sender.OnAck(InputAckMessage{2, 0}));
    EXPECT_FALSE(sender.OnAck(InputAckMessage{0, 0}));
    EXPECT_EQ(sender.Unacked(), 1u);
    EXPECT_EQ(sender.Stats().acksInvalid, 4u);
    EXPECT_TRUE(sender.OnAck(InputAckMessage{1, 0}));
    EXPECT_EQ(sender.Unacked(), 0u);
}

TEST(ClientInputSenderTest, TheRingHoldsAtMostTheCapWhateverIsQueued)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 4);
    for (std::uint32_t tick = 1; tick <= 1000; ++tick)
    {
        ASSERT_TRUE(sender.Queue(tick, Values(0, 0, 0, 0)).has_value());
        EXPECT_LE(sender.Unacked(), kMaxUnackedInputFrames);
    }
    EXPECT_EQ(sender.Unacked(), kMaxUnackedInputFrames);
    EXPECT_EQ(sender.Stats().framesDroppedFull, 1000u - kMaxUnackedInputFrames);
    // The newest are the ones kept
    const auto frames = DecodeInputFrames(sender.BuildMessage(1000).value(), layout.PayloadBytes());
    EXPECT_EQ(frames->back().sequence, 1000u);
}

TEST(ClientInputSenderTest, QueueChecksTheInput)
{
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 2);
    EXPECT_FALSE(sender.Queue(1, std::vector<float>{1.0f}).has_value());
    ASSERT_TRUE(sender.Queue(10, Values(0, 0, 0, 0)).has_value());
    EXPECT_FALSE(sender.Queue(10, Values(0, 0, 0, 0)).has_value()); // ticks must rise
    EXPECT_FALSE(sender.Queue(9, Values(0, 0, 0, 0)).has_value());
    EXPECT_TRUE(sender.Queue(11, Values(0, 0, 0, 0)).has_value());
    ClientInputSender none;
    EXPECT_FALSE(none.Queue(1, std::vector<float>{}).has_value());
}

TEST(ClientInputSenderTest, RedundancyIsClampedAndTheMessageSizeIsRespected)
{
    const InputLayout layout = GameLayout();
    EXPECT_EQ(ClientInputSender(layout, 0).Redundancy(), 1u);
    EXPECT_EQ(ClientInputSender(layout, 100).Redundancy(), kMaxInputFramesPerMessage);

    ClientInputSender sender(layout, 8);
    for (std::uint32_t tick = 1; tick <= 8; ++tick)
    {
        ASSERT_TRUE(sender.Queue(tick, Values(0, 0, 0, 0)).has_value());
    }
    // Room for two frames: 2 + 2 * (8 + 3) = 24 bytes
    const auto small = sender.BuildMessage(24);
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(small->size(), 24u);
    EXPECT_EQ(DecodeInputFrames(*small, layout.PayloadBytes())->size(), 2u);
    // Not even one frame fits
    ASSERT_TRUE(sender.Queue(9, Values(0, 0, 0, 0)).has_value());
    EXPECT_FALSE(sender.BuildMessage(12).has_value());
    EXPECT_EQ(sender.Stats().messagesTooLarge, 1u);
}

TEST(ClientInputSenderTest, TheSequenceSkipsZeroWhenItWraps)
{
    // 2^32 frames cannot be queued in a test; the wrap itself is covered by the decode and buffer tests above. Here: the
    // first sequence is 1 and they rise by one.
    const InputLayout layout = GameLayout();
    ClientInputSender sender(layout, 1);
    EXPECT_EQ(sender.Queue(1, Values(0, 0, 0, 0)).value(), 1u);
    EXPECT_EQ(sender.Queue(2, Values(0, 0, 0, 0)).value(), 2u);
}
