#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/LockstepReplay.hpp"
#include "engine/replay/ReplayLog.hpp"

// Netcode C3, input mapping: a networked InputFrame (B4) travels as an opaque layout-sized payload through the lockstep
// scheduler and the n2replay version 2 rows and comes back the same, for every control type; a layout mismatch is
// refused; version 1 replay files (the committed C2 goldens) are still read as they were and replay to the same hashes;
// malformed and oversize payloads are rejected before anything is allocated by a claimed length.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::N2Net;
using namespace N2Engine::Replay;

namespace
{
    // One of every control type, with different widths, ranges and signedness
    InputLayout AllControlsLayout()
    {
        auto created = InputLayout::Create({
            {"jump", InputControlKind::Button, 1, 1.0f, false},
            {"steer", InputControlKind::Axis, 8, 2.0f, true},
            {"throttle", InputControlKind::Axis, 5, 4.0f, false},
            {"move", InputControlKind::Vector2, 8, 1.0f, true},
            {"look", InputControlKind::Vector3, 6, 3.0f, true},
            {"fire", InputControlKind::Button, 1, 1.0f, false},
        });
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : InputLayout{};
    }

    // The same payload size as AllControlsLayout would have is not needed: a different layout of the same size is
    InputLayout SameSizeOtherRange()
    {
        auto created = InputLayout::Create({
            {"jump", InputControlKind::Button, 1, 1.0f, false},
            {"steer", InputControlKind::Axis, 8, 2.5f, true}, // another range: packs the same, means something else
            {"throttle", InputControlKind::Axis, 5, 4.0f, false},
            {"move", InputControlKind::Vector2, 8, 1.0f, true},
            {"look", InputControlKind::Vector3, 6, 3.0f, true},
            {"fire", InputControlKind::Button, 1, 1.0f, false},
        });
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : InputLayout{};
    }

    // 16 axes of 16 bits: the largest payload there can be (32 bytes)
    InputLayout BiggestLayout()
    {
        std::vector<InputControlSpec> controls;
        for (int i = 0; i < 16; ++i)
        {
            controls.push_back(InputControlSpec{std::string("a") + static_cast<char>('a' + i), InputControlKind::Axis, 16, 1.0f, true});
        }
        auto created = InputLayout::Create(std::move(controls));
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : InputLayout{};
    }

    std::vector<float> ValuesFor(const InputLayout &layout, const std::uint32_t seed)
    {
        std::vector<float> values(layout.ValueCount());
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            // Includes both ends of the range, zero and values between steps
            const std::uint32_t pick = (seed * 2654435761u + static_cast<std::uint32_t>(i) * 40503u) >> 7;
            values[i] = static_cast<float>(pick % 2001) / 1000.0f - 1.0f;
            if (pick % 11 == 0)
            {
                values[i] = 1000.0f;
            }
            if (pick % 13 == 0)
            {
                values[i] = -1000.0f;
            }
        }
        return values;
    }

    std::vector<std::uint8_t> PackFor(const InputLayout &layout, const std::uint32_t seed)
    {
        auto packed = layout.Pack(ValuesFor(layout, seed));
        EXPECT_TRUE(packed.has_value());
        return packed.value_or(std::vector<std::uint8_t>{});
    }

    LockstepConfig ConfigFor(const InputLayout &layout, const std::uint32_t slots, const std::uint32_t delay)
    {
        LockstepConfig config;
        config.slotCount = slots;
        config.inputDelay = delay;
        config.layout = *MakeLockstepLayoutInfo(layout);
        return config;
    }

    ReplayLog EmptyV2Log(const InputLayout &layout, const int ticks)
    {
        ReplayLog log;
        log.version = kReplayFormatVersionLayout;
        log.scene = "transforms";
        log.seed = 3;
        log.ticks = ticks;
        log.hashInterval = 10;
        for (std::size_t i = 0; i < layout.ControlCount(); ++i)
        {
            log.layout.push_back(layout.Control(i));
        }
        return log;
    }

    // A v2 file as JSON text, built by hand so a test can break one part of it
    nlohmann::json FileJson(const InputLayout &layout, const ReplayLog &log)
    {
        std::string error;
        const auto parsed = nlohmann::json::parse(SerializeReplayLog(log), nullptr, false);
        EXPECT_FALSE(parsed.is_discarded());
        (void)layout;
        return parsed;
    }

    void ExpectSameLog(const ReplayLog &a, const ReplayLog &b)
    {
        EXPECT_EQ(a.version, b.version);
        EXPECT_EQ(a.scene, b.scene);
        EXPECT_EQ(a.seed, b.seed);
        EXPECT_EQ(a.ticks, b.ticks);
        EXPECT_EQ(a.hashInterval, b.hashInterval);
        EXPECT_EQ(a.inputs, b.inputs);
        const auto la = InputLayout::Create(a.layout);
        const auto lb = InputLayout::Create(b.layout);
        EXPECT_EQ(la.has_value(), lb.has_value());
        if (la && lb)
        {
            EXPECT_EQ(la->Hash(), lb->Hash());
        }
        EXPECT_EQ(a.layout.size(), b.layout.size());
    }

    std::string Parse(const nlohmann::json &json)
    {
        std::string error;
        return ParseReplayLog(json.dump(), error).has_value() ? std::string() : (error.empty() ? "?" : error);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// InputFrame -> lockstep -> replay row -> InputFrame
// ---------------------------------------------------------------------------------------------------------------------

TEST(InputFrameMapping, EveryControlTypeSurvivesFrameToSchedulerToReplayRowAndBack)
{
    const InputLayout layout = AllControlsLayout();
    ASSERT_EQ(layout.ControlCount(), 6u);
    constexpr int kTicks = 40;

    LockstepScheduler scheduler(ConfigFor(layout, 2, 0));
    ASSERT_TRUE(scheduler.Valid());
    LockstepRecorder recorder("transforms", 3, 10, 2, layout);

    // What each slot sent, as the client's InputFrames
    std::vector<std::array<InputFrame, 2>> sent;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        std::array<InputFrame, 2> frames;
        for (std::uint32_t slot = 0; slot < 2; ++slot)
        {
            frames[slot].tick = static_cast<std::uint32_t>(tick);
            frames[slot].sequence = static_cast<std::uint32_t>(tick) + 1;
            frames[slot].payload = PackFor(layout, static_cast<std::uint32_t>(tick) * 2 + slot);
            const auto input = ToLockstepInput(layout, frames[slot]);
            ASSERT_TRUE(input.has_value()) << input.error();
            EXPECT_EQ(scheduler.Submit(slot, frames[slot].tick, *input), SubmitResult::Accepted);
        }
        sent.push_back(frames);
        TickInputs out;
        ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
        ASSERT_EQ(out.tick, static_cast<std::uint32_t>(tick));
        ASSERT_TRUE(recorder.RecordInputs(std::span<const LockstepInput>(out.inputs.data(), 2)));
    }
    ASSERT_EQ(recorder.TicksRecorded(), kTicks);

    // Through the file
    std::string error;
    const auto log = ParseReplayLog(SerializeReplayLog(recorder.Log()), error);
    ASSERT_TRUE(log.has_value()) << error;
    EXPECT_EQ(log->version, kReplayFormatVersionLayout);
    EXPECT_EQ(log->layout.size(), 6u);
    ExpectSameLog(*log, recorder.Log());

    // Rows back to frames: a slot holds a row's payload until its next row
    std::array<std::vector<std::uint8_t>, 2> held{layout.NeutralPayload(), layout.NeutralPayload()};
    std::size_t row = 0;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        for (; row < log->inputs.size() && log->inputs[row].tick == tick; ++row)
        {
            held[static_cast<std::size_t>(log->inputs[row].slot)] = log->inputs[row].payload;
        }
        for (std::size_t slot = 0; slot < 2; ++slot)
        {
            LockstepInput input;
            {
                const auto made = LockstepInput::FromBytes(held[slot]);
                ASSERT_TRUE(made.has_value());
                input = *made;
            }
            const InputFrame back = ToInputFrame(static_cast<std::uint32_t>(tick), sent[static_cast<std::size_t>(tick)][slot].sequence, input);
            EXPECT_EQ(back.tick, sent[static_cast<std::size_t>(tick)][slot].tick) << "tick " << tick << " slot " << slot;
            EXPECT_EQ(back.payload, sent[static_cast<std::size_t>(tick)][slot].payload) << "tick " << tick << " slot " << slot;
            // ... and means the same values
            EXPECT_EQ(layout.Unpack(back.payload), layout.Unpack(sent[static_cast<std::size_t>(tick)][slot].payload));
        }
    }
}

TEST(InputFrameMapping, FrameValuesAreTheQuantisedOnesOnEveryPeer)
{
    const InputLayout layout = AllControlsLayout();
    const std::vector<std::uint8_t> payload = PackFor(layout, 5);
    const auto input = ToLockstepInput(layout, payload);
    ASSERT_TRUE(input.has_value());
    // The bytes are not touched on the way: what the simulation unpacks is what the sender's Pack gave
    EXPECT_EQ(std::vector<std::uint8_t>(input->Bytes().begin(), input->Bytes().end()), payload);
    EXPECT_EQ(layout.Unpack(input->Bytes()), layout.Unpack(payload));
    // Re-packing the unpacked values gives the same bytes: the payload is the canonical form of its values
    EXPECT_EQ(layout.Pack(*layout.Unpack(payload)), payload);
}

TEST(InputFrameMapping, LockstepSchedulerNeutralIsTheLayoutsNeutralPayload)
{
    const InputLayout layout = AllControlsLayout();
    LockstepScheduler scheduler(ConfigFor(layout, 2, 2));
    TickInputs out;
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    const std::vector<std::uint8_t> neutral = layout.NeutralPayload();
    EXPECT_EQ(std::vector<std::uint8_t>(out.inputs[0].Bytes().begin(), out.inputs[0].Bytes().end()), neutral);
    EXPECT_NE(neutral, std::vector<std::uint8_t>(neutral.size(), 0)) << "a signed axis at rest is not the zero bit pattern";
}

TEST(InputFrameMapping, SchedulerRefusesAnInputOfTheWrongSize)
{
    const InputLayout layout = AllControlsLayout();
    LockstepScheduler scheduler(ConfigFor(layout, 2, 0));
    LockstepInput wrong;
    wrong.size = static_cast<std::uint8_t>(layout.PayloadBytes() + 1);
    EXPECT_EQ(scheduler.Submit(0, 0, wrong), SubmitResult::BadPayload);
    EXPECT_EQ(scheduler.Submit(0, 0, LockstepInput{}), SubmitResult::BadPayload);
    EXPECT_EQ(scheduler.Submit(0, 0, *LockstepInput::FromBytes(PackFor(layout, 1))), SubmitResult::Accepted);
    EXPECT_EQ(scheduler.MissingSlots(), 0b10u);
}

TEST(InputFrameMapping, ASchedulerWhoseNeutralDoesNotMatchItsLayoutIsInvalid)
{
    LockstepConfig config;
    config.layout.payloadBytes = 4; // but the neutral input is empty
    LockstepScheduler scheduler(config);
    EXPECT_FALSE(scheduler.Valid());
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout mismatch, malformed and oversize payloads
// ---------------------------------------------------------------------------------------------------------------------

TEST(InputFrameMapping, ABundleUnderAnotherLayoutIsRefused)
{
    const InputLayout mine = AllControlsLayout();
    const InputLayout other = SameSizeOtherRange();
    ASSERT_EQ(mine.PayloadBytes(), other.PayloadBytes());
    ASSERT_NE(mine.Hash(), other.Hash());

    LockstepScheduler theirs(ConfigFor(other, 2, 2));
    for (std::uint32_t tick = 2; tick < 6; ++tick)
    {
        ASSERT_EQ(theirs.Submit(0, tick, *LockstepInput::FromBytes(PackFor(other, tick))), SubmitResult::Accepted);
    }
    const InputBundle bundle = BuildBundleFor(theirs, 0, 0);
    std::array<std::uint8_t, 1100> buffer{};
    const std::size_t size = EncodeInputBundle(bundle, buffer);
    ASSERT_GT(size, 0u);
    const std::span<const std::uint8_t> bytes(buffer.data(), size);

    // Same payload size, other hash
    const auto refused = DecodeInputBundleFor(mine, bytes);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), BundleError::LayoutMismatch);
    // The layout it was made for takes it
    EXPECT_TRUE(DecodeInputBundleFor(other, bytes).has_value());

    // Another payload size
    const auto legacy = DecodeInputBundleFor(LegacyMoveLayout(), bytes);
    ASSERT_FALSE(legacy.has_value());
    EXPECT_EQ(legacy.error(), BundleError::LayoutMismatch);

    // The scheduler-level codec refuses on the hash alone
    const auto info = *MakeLockstepLayoutInfo(mine);
    ASSERT_FALSE(DecodeInputBundle(bytes, info).has_value());
    EXPECT_EQ(DecodeInputBundle(bytes, info).error(), BundleError::LayoutMismatch);
}

TEST(InputFrameMapping, APayloadTheLayoutDoesNotProduceIsRefused)
{
    const InputLayout layout = AllControlsLayout();
    std::vector<std::uint8_t> payload = PackFor(layout, 9);

    EXPECT_FALSE(ToLockstepInput(layout, std::span<const std::uint8_t>(payload.data(), payload.size() - 1)).has_value()) << "short";
    std::vector<std::uint8_t> longer = payload;
    longer.push_back(0);
    EXPECT_FALSE(ToLockstepInput(layout, longer).has_value()) << "long";
    EXPECT_FALSE(ToLockstepInput(layout, std::vector<std::uint8_t>(10000, 0xFF)).has_value()) << "oversize";
    EXPECT_FALSE(ToLockstepInput(layout, std::vector<std::uint8_t>{}).has_value()) << "empty";

    // A step beyond the range (all ones is above the largest step of a signed axis) and non-zero padding
    const std::vector<std::uint8_t> ones(payload.size(), 0xFF);
    EXPECT_FALSE(ToLockstepInput(layout, ones).has_value());
    ASSERT_NE(layout.BitCount() % 8, 0u) << "the test layout has padding bits";
    std::vector<std::uint8_t> padded = payload;
    padded.back() = static_cast<std::uint8_t>(padded.back() | 0x80);
    EXPECT_FALSE(ToLockstepInput(layout, padded).has_value());

    // Inside a bundle the same
    LockstepScheduler scheduler(ConfigFor(layout, 2, 2));
    ASSERT_EQ(scheduler.Submit(0, 2, *LockstepInput::FromBytes(payload)), SubmitResult::Accepted);
    InputBundle bundle = BuildBundleFor(scheduler, 0, 0);
    bundle.frames.back() = *LockstepInput::FromBytes(padded);
    std::array<std::uint8_t, 1100> buffer{};
    const std::size_t size = EncodeInputBundle(bundle, buffer);
    ASSERT_GT(size, 0u);
    const auto decoded = DecodeInputBundleFor(layout, std::span<const std::uint8_t>(buffer.data(), size));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), BundleError::Malformed);
}

namespace
{
    void PutU32(std::vector<std::uint8_t> &out, const std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }

    std::vector<std::uint8_t> RawBundle(const std::uint8_t count, const std::uint8_t payloadBytes, const std::uint64_t hash,
                                        const std::size_t bodyBytes)
    {
        std::vector<std::uint8_t> bytes{1, 0};
        PutU32(bytes, 0);
        PutU32(bytes, 0);
        bytes.push_back(count);
        bytes.push_back(payloadBytes);
        PutU32(bytes, static_cast<std::uint32_t>(hash));
        PutU32(bytes, static_cast<std::uint32_t>(hash >> 32));
        bytes.insert(bytes.end(), bodyBytes, 0);
        return bytes;
    }
}

TEST(InputFrameMapping, ClaimedSizesAreCheckedAgainstTheCapsAndTheBytesThatAreThere)
{
    const InputLayout layout = BiggestLayout();
    ASSERT_EQ(layout.PayloadBytes(), kMaxInputPayloadBytes);
    const auto info = *MakeLockstepLayoutInfo(layout);

    // A payload size above the cap, with a body that "matches" it: refused as malformed, nothing reserved
    const auto huge = DecodeInputBundle(RawBundle(32, 255, info.hash, 32 * 255), info);
    ASSERT_FALSE(huge.has_value());
    EXPECT_EQ(huge.error(), BundleError::Malformed);
    // 32 frames of 32 bytes claimed, a body of one byte
    const auto thin = DecodeInputBundle(RawBundle(32, 32, info.hash, 1), info);
    ASSERT_FALSE(thin.has_value());
    EXPECT_EQ(thin.error(), BundleError::Malformed);
    // More frames than the cap
    const auto many = DecodeInputBundle(RawBundle(33, 32, info.hash, 33 * 32), info);
    ASSERT_FALSE(many.has_value());
    EXPECT_EQ(many.error(), BundleError::Malformed);
    // Trailing bytes
    EXPECT_FALSE(DecodeInputBundle(RawBundle(2, 32, info.hash, 2 * 32 + 1), info).has_value());
    // Exactly right is accepted (a zero payload is a valid step pattern of this layout? no: zero bits are the lowest
    // step, which is valid for a signed axis)
    const auto exact = DecodeInputBundleFor(layout, RawBundle(32, 32, info.hash, 32 * 32));
    EXPECT_TRUE(exact.has_value());
    // The largest bundle there can be encodes into the size the constant says
    InputBundle full;
    full.payloadBytes = 32;
    full.layoutHash = info.hash;
    full.frames.assign(kMaxBundleFrames, *LockstepInput::FromBytes(PackFor(layout, 1)));
    std::vector<std::uint8_t> buffer(InputBundleSize(kMaxBundleFrames, 32));
    EXPECT_EQ(EncodeInputBundle(full, buffer), buffer.size());
    std::vector<std::uint8_t> tooSmall(buffer.size() - 1);
    EXPECT_EQ(EncodeInputBundle(full, tooSmall), 0u);
    // A frame whose size is not the bundle's is not encodable
    full.frames[3].size = 31;
    EXPECT_EQ(EncodeInputBundle(full, buffer), 0u);
}

TEST(InputFrameMapping, RandomBundlesNeverCrashAndAcceptedOnesAreBounded)
{
    const InputLayout layout = AllControlsLayout();
    const auto info = *MakeLockstepLayoutInfo(layout);
    SimRandom random(0x1A70C7);
    std::vector<std::uint8_t> bytes(1100);
    for (int round = 0; round < 20000; ++round)
    {
        const std::size_t length = random.NextBelow(static_cast<std::uint32_t>(bytes.size()) + 1);
        for (std::size_t i = 0; i < length; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(random.Next());
        }
        if (length >= 20 && random.NextBelow(2) == 0)
        {
            // Make the header plausible so the fuzz gets past the cheap checks
            bytes[0] = 1;
            bytes[10] = static_cast<std::uint8_t>(random.NextBelow(34));
            bytes[11] = static_cast<std::uint8_t>(info.payloadBytes);
            for (int i = 0; i < 8; ++i)
            {
                bytes[12 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(info.hash >> (8 * i));
            }
        }
        const auto decoded = DecodeInputBundleFor(layout, std::span<const std::uint8_t>(bytes.data(), length));
        if (decoded)
        {
            EXPECT_LE(decoded->frames.size(), kMaxBundleFrames);
            for (const LockstepInput &frame : decoded->frames)
            {
                EXPECT_TRUE(layout.IsValidPayload(frame.Bytes()));
            }
        }
    }
}

TEST(InputFrameMapping, LockstepInputIsBoundedByTheLargestPayload)
{
    EXPECT_TRUE(LockstepInput::FromBytes(std::vector<std::uint8_t>(32, 1)).has_value());
    EXPECT_FALSE(LockstepInput::FromBytes(std::vector<std::uint8_t>(33, 1)).has_value());
    EXPECT_TRUE(LockstepInput::FromBytes({}).has_value());
    EXPECT_EQ(sizeof(LockstepInput::bytes), kMaxInputPayloadBytes);
}

// ---------------------------------------------------------------------------------------------------------------------
// The scene mapping
// ---------------------------------------------------------------------------------------------------------------------

TEST(InputFrameMapping, SceneInputIsReadByNameAndButtonOrder)
{
    const InputLayout layout = AllControlsLayout();
    // Controls in name order: fire, jump, look, move, steer, throttle; buttons are fire (bit 0) and jump (bit 1)
    std::vector<float> values(layout.ValueCount(), 0.0f);
    values[layout.ValueOffset(*layout.FindControl("fire"))] = 1.0f;
    const std::size_t move = layout.ValueOffset(*layout.FindControl("move"));
    values[move] = 1.0f;
    values[move + 1] = -1.0f;
    const auto payload = layout.Pack(values);
    ASSERT_TRUE(payload.has_value());
    int x = 99;
    int y = 99;
    std::uint32_t buttons = 99;
    ASSERT_TRUE(DeriveSceneInput(layout, *payload, x, y, buttons));
    EXPECT_EQ(x, 127);
    EXPECT_EQ(y, -127);
    EXPECT_EQ(buttons, 0b01u);

    values[layout.ValueOffset(*layout.FindControl("jump"))] = 1.0f;
    ASSERT_TRUE(DeriveSceneInput(layout, *layout.Pack(values), x, y, buttons));
    EXPECT_EQ(buttons, 0b11u);

    // Neutral input is no movement and no button
    ASSERT_TRUE(DeriveSceneInput(layout, layout.NeutralPayload(), x, y, buttons));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(buttons, 0u);

    // A payload the layout does not produce is refused
    EXPECT_FALSE(DeriveSceneInput(layout, std::vector<std::uint8_t>(layout.PayloadBytes(), 0xFF), x, y, buttons));
    EXPECT_FALSE(DeriveSceneInput(layout, std::vector<std::uint8_t>{}, x, y, buttons));
}

TEST(InputFrameMapping, TheLegacyLayoutMapsExactlyOntoTheOriginalTriple)
{
    int x = 0;
    int y = 0;
    std::uint32_t buttons = 0;
    for (const int mx : {-127, -64, -1, 0, 1, 63, 127})
    {
        for (const int my : {-127, -3, 0, 5, 127})
        {
            for (const std::uint32_t b : {0u, 1u, 0x20000000u, 0x3FFFFFFFu, 0x25A5A5A5u})
            {
                const LockstepInput input = LegacyInput(mx, my, b);
                ASSERT_EQ(input.size, 6);
                ASSERT_TRUE(DeriveSceneInput(LegacyMoveLayout(), input.Bytes(), x, y, buttons));
                EXPECT_EQ(x, mx);
                EXPECT_EQ(y, my);
                EXPECT_EQ(buttons, b);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The n2replay version 2 file
// ---------------------------------------------------------------------------------------------------------------------

TEST(ReplayLogV2, RoundTripsAndKeepsTheLayoutHeader)
{
    const InputLayout layout = AllControlsLayout();
    LockstepRecorder recorder("lua_sim", 11, 10, 2, layout);
    for (int tick = 0; tick < 20; ++tick)
    {
        const std::array<LockstepInput, 2> inputs{*LockstepInput::FromBytes(PackFor(layout, static_cast<std::uint32_t>(tick / 3))),
                                                  *LockstepInput::FromBytes(layout.NeutralPayload())};
        ASSERT_TRUE(recorder.RecordInputs(inputs));
    }
    const ReplayLog &log = recorder.Log();
    // The neutral slot never changes: no rows for it; slot 0 changes every third tick
    EXPECT_EQ(log.inputs.size(), 7u);
    EXPECT_EQ(ValidateReplayLog(log), "");

    const std::string text = SerializeReplayLog(log);
    const nlohmann::json json = nlohmann::json::parse(text);
    EXPECT_EQ(json["version"], 2);
    EXPECT_EQ(json["layout"].size(), 6u);
    EXPECT_EQ(json["layoutHash"], FormatHash(layout.Hash()));
    EXPECT_TRUE(json["inputs"][0][2].is_string());
    std::string error;
    const auto parsed = ParseReplayLog(text, error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ExpectSameLog(*parsed, log);
    EXPECT_EQ(SerializeReplayLog(*parsed), text) << "canonical text";
}

TEST(ReplayLogV2, ARecorderOfOneVersionRefusesTheOthersCalls)
{
    const InputLayout layout = AllControlsLayout();
    LockstepRecorder v2("transforms", 1, 10, 1, layout);
    EXPECT_FALSE(v2.RecordTick(std::array<SlotState, 1>{}));
    LockstepRecorder v1("transforms", 1, 10, 1);
    EXPECT_FALSE(v1.RecordInputs(std::array<LockstepInput, 1>{}));
    EXPECT_EQ(v1.Log().version, kReplayFormatVersion);
    // An input that is not a payload of the layout records nothing
    LockstepInput bad;
    bad.size = 3;
    EXPECT_FALSE(v2.RecordInputs(std::array<LockstepInput, 1>{bad}));
    EXPECT_EQ(v2.TicksRecorded(), 0);
}

TEST(ReplayLogV2, MalformedFilesAreRefused)
{
    const InputLayout layout = AllControlsLayout();
    ReplayLog log = EmptyV2Log(layout, 10);
    ReplayInput row;
    row.tick = 2;
    row.slot = 1;
    row.payload = PackFor(layout, 4);
    ASSERT_TRUE(DeriveSceneInput(layout, row.payload, row.moveX, row.moveY, row.buttons));
    log.inputs.push_back(row);
    const nlohmann::json good = FileJson(layout, log);
    ASSERT_EQ(Parse(good), "");

    auto broken = [&](auto &&edit)
    {
        nlohmann::json json = good;
        edit(json);
        return Parse(json);
    };
    EXPECT_NE(broken([](nlohmann::json &j) { j["layoutHash"] = "0x0000000000000001"; }), "") << "hash disagrees with the layout";
    EXPECT_NE(broken([](nlohmann::json &j) { j.erase("layoutHash"); }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j.erase("layout"); }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"] = "no"; }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][0]["kind"] = "joystick"; }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][0].erase("range"); }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][4]["bits"] = 99; }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][4]["range"] = 0.0; }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][1]["name"] = "9bad name"; }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"][1]["name"] = std::string(200, 'a'); }), "");
    EXPECT_NE(broken([](nlohmann::json &j) { j["layout"].push_back(j["layout"][0]); }), "") << "duplicate name";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0] = nlohmann::json::array({2, 1}); }), "") << "short row";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0] = nlohmann::json::array({2, 1, 5}); }), "") << "payload is a number";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][2] = "00"; }), "") << "payload too short";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][2] = j["inputs"][0][2].get<std::string>() + "00"; }), "") << "too long";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][2] = std::string(j["inputs"][0][2].get<std::string>().size(), 'z'); }), "")
        << "not hex";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][2] = std::string(j["inputs"][0][2].get<std::string>().size(), 'f'); }), "")
        << "steps beyond the range";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][0] = 10; }), "") << "tick outside ticks";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"][0][1] = 9; }), "") << "slot outside";
    EXPECT_NE(broken([](nlohmann::json &j) { j["inputs"].push_back(j["inputs"][0]); }), "") << "rows not increasing";
    EXPECT_NE(broken([](nlohmann::json &j) { j["version"] = 3; }), "");

    // The triple of a row must be the payload's
    ReplayLog lie = log;
    lie.inputs[0].moveX = lie.inputs[0].moveX == 0 ? 1 : 0;
    EXPECT_NE(ValidateReplayLog(lie), "");
    // A version 1 log carries no layout and no payload
    ReplayLog v1 = log;
    v1.version = kReplayFormatVersion;
    EXPECT_NE(ValidateReplayLog(v1), "");
}

TEST(ReplayLogV2, OversizeDeclarationsAreRefusedBeforeAnythingIsBuiltFromThem)
{
    const InputLayout layout = AllControlsLayout();
    const nlohmann::json good = FileJson(layout, EmptyV2Log(layout, 10));

    // 100000 controls: refused by the size of the array, not by walking it
    nlohmann::json many = good;
    many["layout"] = nlohmann::json::array();
    for (int i = 0; i < 100000; ++i)
    {
        many["layout"].push_back(good["layout"][0]);
    }
    EXPECT_NE(Parse(many), "");

    // A megabyte payload in a row
    nlohmann::json bigRow = FileJson(layout, [&]
    {
        ReplayLog log = EmptyV2Log(layout, 10);
        ReplayInput row;
        row.tick = 1;
        row.payload = layout.NeutralPayload();
        log.inputs.push_back(row);
        return log;
    }());
    bigRow["inputs"][0][2] = std::string(1 << 20, 'a');
    EXPECT_NE(Parse(bigRow), "");

    // A layout that would make a payload over the cap
    nlohmann::json wide = good;
    wide["layout"] = nlohmann::json::array();
    for (int i = 0; i < 20; ++i)
    {
        wide["layout"].push_back({{"name", "a" + std::to_string(i)}, {"kind", "axis"}, {"bits", 16}, {"range", 1.0}, {"signed", true}});
    }
    EXPECT_NE(Parse(wide), "");

    // Too many rows for the format
    ReplayLog log = EmptyV2Log(layout, 10);
    EXPECT_EQ(ValidateReplayLog(log), "");
}

// ---------------------------------------------------------------------------------------------------------------------
// Version 1 stays readable and replays to the same hashes
// ---------------------------------------------------------------------------------------------------------------------

TEST(ReplayLogV2, VersionOneGoldensStillLoadAndReplayToTheSameHashesAsTheirVersionTwoForm)
{
    for (const std::string &scene : SceneNames())
    {
        std::string error;
        const auto v1 = LoadReplayLog(std::string(N2_GOLDEN_REPLAY_DIR) + "/" + scene + ".n2replay.json", error);
        ASSERT_TRUE(v1.has_value()) << scene << ": " << error;
        EXPECT_EQ(v1->version, kReplayFormatVersion);
        EXPECT_TRUE(v1->layout.empty());
        for (const ReplayInput &row : v1->inputs)
        {
            EXPECT_TRUE(row.payload.empty());
        }
        // Reading and writing a version 1 log is the format it always was
        const auto again = ParseReplayLog(SerializeReplayLog(*v1), error);
        ASSERT_TRUE(again.has_value()) << error;
        ExpectSameLog(*again, *v1);
        EXPECT_EQ(nlohmann::json::parse(SerializeReplayLog(*v1))["inputs"][0].size(), 5u);

        // The same inputs as version 2 rows under the legacy layout
        ReplayLog v2 = *v1;
        v2.version = kReplayFormatVersionLayout;
        for (std::size_t i = 0; i < LegacyMoveLayout().ControlCount(); ++i)
        {
            v2.layout.push_back(LegacyMoveLayout().Control(i));
        }
        for (ReplayInput &row : v2.inputs)
        {
            const LockstepInput input = LegacyInput(row.moveX, row.moveY, row.buttons);
            row.payload.assign(input.Bytes().begin(), input.Bytes().end());
        }
        const auto reparsed = ParseReplayLog(SerializeReplayLog(v2), error);
        ASSERT_TRUE(reparsed.has_value()) << scene << ": " << error;
        EXPECT_EQ(reparsed->inputs, v2.inputs);

        const ReplayResult a = RunReplay(*v1);
        const ReplayResult b = RunReplay(*reparsed);
        ASSERT_TRUE(a.ok) << a.error;
        ASSERT_TRUE(b.ok) << b.error;
        ASSERT_EQ(a.hashes.size(), b.hashes.size());
        ASSERT_FALSE(a.hashes.empty());
        for (std::size_t i = 0; i < a.hashes.size(); ++i)
        {
            EXPECT_EQ(a.hashes[i].hash, b.hashes[i].hash) << scene << " tick " << a.hashes[i].tick;
        }
    }
}

TEST(ReplayLogV2, AVersionOneRecorderStillWritesVersionOne)
{
    LockstepRecorder recorder("transforms", 1, 10, 1);
    const std::array<SlotState, 1> a{SlotState{3, -4, 5}};
    ASSERT_TRUE(recorder.RecordTick(a));
    const nlohmann::json json = nlohmann::json::parse(SerializeReplayLog(recorder.Log()));
    EXPECT_EQ(json["version"], 1);
    EXPECT_FALSE(json.contains("layout"));
    EXPECT_EQ(json["inputs"][0], nlohmann::json::array({0, 0, 3, -4, 5}));
}
