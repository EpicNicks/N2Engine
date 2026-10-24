#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/network/n2net/SimTransport.hpp"

// The lockstep input scheduler (netcode C3): input delay, the "every slot known" gate, duplicate / conflicting / late /
// far-future inputs, the stall timeout, dropping a slot, 32-bit tick wraparound, the bounded input bundle codec (including
// hostile bytes), and three peers running the scheduler over lossy, reordering, duplicating SimTransport links.

using namespace N2Engine::N2Net;

namespace
{
    // The scheduler is layout-agnostic; these tests run it under the legacy (moveX, moveY, buttons) layout, six bytes
    const LockstepLayoutInfo &Layout()
    {
        static const LockstepLayoutInfo info = *N2Engine::Network::MakeLockstepLayoutInfo(N2Engine::Network::LegacyMoveLayout());
        return info;
    }

    const LockstepInput &Neutral() { return Layout().neutral; }

    LockstepInput In(const int x, const int y = 0, const std::uint32_t buttons = 0)
    {
        return N2Engine::Network::LegacyInput(x, y, buttons);
    }

    InputBundle MakeBundle(const std::size_t frames)
    {
        InputBundle bundle;
        bundle.payloadBytes = Layout().payloadBytes;
        bundle.layoutHash = Layout().hash;
        for (std::size_t i = 0; i < frames; ++i)
        {
            bundle.frames.push_back(In(static_cast<int>(i)));
        }
        return bundle;
    }

    LockstepConfig Config(const std::uint32_t slots = 2, const std::uint32_t delay = 2)
    {
        LockstepConfig config;
        config.layout = Layout();
        config.slotCount = slots;
        config.inputDelay = delay;
        return config;
    }
}

TEST(LockstepScheduler, InvalidConfigRefusesEverything)
{
    LockstepScheduler zero(Config(0));
    EXPECT_FALSE(zero.Valid());
    LockstepScheduler many(Config(kMaxLockstepSlots + 1));
    EXPECT_FALSE(many.Valid());
    LockstepScheduler slow(Config(2, kMaxInputDelay + 1));
    EXPECT_FALSE(slow.Valid());
    EXPECT_EQ(slow.Submit(0, 0, In(1)), SubmitResult::BadSlot);
    TickInputs out;
    EXPECT_EQ(slow.TryAdvance(out), AdvanceStatus::Waiting);
}

TEST(LockstepScheduler, FirstTicksAreNeutralThenWaitsForEverySlot)
{
    LockstepScheduler scheduler(Config(2, 2));
    TickInputs out;
    // Ticks 0 and 1 have no possible sender: neutral for everyone
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(out.tick, 0u);
    EXPECT_EQ(out.inputs[0], Neutral());
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(out.tick, 1u);
    // Tick 2 needs both slots
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Waiting);
    EXPECT_EQ(scheduler.MissingSlots(), 0b11u);
    EXPECT_EQ(scheduler.Submit(0, 2, In(5)), SubmitResult::Accepted);
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Waiting);
    EXPECT_EQ(scheduler.MissingSlots(), 0b10u);
    EXPECT_EQ(scheduler.Submit(1, 2, In(-6, 7, 0x80)), SubmitResult::Accepted);
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(out.tick, 2u);
    EXPECT_EQ(out.inputs[0], In(5));
    EXPECT_EQ(out.inputs[1], In(-6, 7, 0x80));
    EXPECT_EQ(scheduler.CurrentTick(), 3u);
}

TEST(LockstepScheduler, ZeroDelayNeedsTheFirstTickToo)
{
    LockstepScheduler scheduler(Config(1, 0));
    TickInputs out;
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Waiting);
    std::uint32_t assigned = 99;
    EXPECT_EQ(scheduler.SubmitLocal(0, In(3), assigned), SubmitResult::Accepted);
    EXPECT_EQ(assigned, 0u);
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
}

TEST(LockstepScheduler, LocalInputsGetCurrentTickPlusDelay)
{
    LockstepScheduler scheduler(Config(1, 3));
    std::uint32_t assigned = 0;
    EXPECT_EQ(scheduler.SubmitLocal(0, In(1), assigned), SubmitResult::Accepted);
    EXPECT_EQ(assigned, 3u);
    EXPECT_EQ(scheduler.SubmitLocal(0, In(2), assigned), SubmitResult::Accepted);
    EXPECT_EQ(assigned, 4u);
    TickInputs out;
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    }
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(out.tick, 3u);
    EXPECT_EQ(out.inputs[0], In(1));
    ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(out.inputs[0], In(2));
    EXPECT_EQ(scheduler.SubmitLocal(1, In(1), assigned), SubmitResult::BadSlot);
}

TEST(LockstepScheduler, LocalInputsStopAtTheWindow)
{
    LockstepScheduler scheduler(Config(1, 2));
    std::uint32_t assigned = 0;
    int accepted = 0;
    while (scheduler.SubmitLocal(0, In(1), assigned) == SubmitResult::Accepted)
    {
        ++accepted;
        ASSERT_LT(accepted, 1000);
    }
    // The window is [0, 128): ticks 2..127 are free
    EXPECT_EQ(accepted, static_cast<int>(kLockstepWindow) - 2);
}

TEST(LockstepScheduler, DuplicateConflictLateAndFarInputs)
{
    LockstepScheduler scheduler(Config(2, 2));
    EXPECT_EQ(scheduler.Submit(1, 4, In(9)), SubmitResult::Accepted);
    EXPECT_EQ(scheduler.Submit(1, 4, In(9)), SubmitResult::Duplicate);
    EXPECT_EQ(scheduler.Submit(1, 4, In(8)), SubmitResult::Conflict);
    EXPECT_EQ(scheduler.Get(1, 4), In(9)) << "the first input stays";
    EXPECT_EQ(scheduler.Submit(2, 4, In(1)), SubmitResult::BadSlot);
    EXPECT_EQ(scheduler.Submit(0, kLockstepWindow, In(1)), SubmitResult::TooFar);
    EXPECT_EQ(scheduler.Submit(0, kLockstepWindow - 1, In(1)), SubmitResult::Accepted);
    EXPECT_EQ(scheduler.Submit(0, 0x7FFFFFFFu, In(1)), SubmitResult::TooFar);
    EXPECT_EQ(scheduler.Submit(0, 0x80000000u, In(1)), SubmitResult::TooOld) << "2^31 behind is the past";

    TickInputs out;
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    // Ticks 0 and 1 ran: a late input for them is refused, and so is a negative-looking tick
    EXPECT_EQ(scheduler.Submit(0, 1, In(1)), SubmitResult::TooOld);
    EXPECT_EQ(scheduler.Submit(0, 0xFFFFFFFFu, In(1)), SubmitResult::TooOld);
}

TEST(LockstepScheduler, OutOfOrderInputsRunInOrder)
{
    LockstepScheduler scheduler(Config(1, 2));
    // 5 before 4 before 3 before 2
    for (std::uint32_t tick = 5; tick >= 2; --tick)
    {
        EXPECT_EQ(scheduler.Submit(0, tick, In(static_cast<int>(tick))), SubmitResult::Accepted);
    }
    TickInputs out;
    std::vector<std::uint32_t> ran;
    while (scheduler.TryAdvance(out) == AdvanceStatus::Runnable)
    {
        ran.push_back(out.tick);
        if (out.tick >= 2)
        {
            EXPECT_EQ(out.inputs[0], In(static_cast<int>(out.tick)));
        }
    }
    EXPECT_EQ(ran, (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
}

TEST(LockstepScheduler, StallTimeoutReportsAndRecovers)
{
    LockstepConfig config = Config(2, 0);
    config.stallTimeoutPolls = 5;
    LockstepScheduler scheduler(config);
    scheduler.Submit(0, 0, In(1));
    TickInputs out;
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Waiting);
    }
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::TimedOut);
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::TimedOut) << "it keeps being reported";
    EXPECT_EQ(scheduler.MissingSlots(), 0b10u);
    scheduler.Submit(1, 0, In(2));
    EXPECT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
    EXPECT_EQ(scheduler.StalledPolls(), 0u);
}

TEST(LockstepScheduler, DroppedSlotIsNeutralOrHoldsItsLastInput)
{
    for (const DropFill fill : {DropFill::Neutral, DropFill::HoldLast})
    {
        LockstepConfig config = Config(2, 0);
        config.dropFill = fill;
        LockstepScheduler scheduler(config);
        TickInputs out;
        scheduler.Submit(0, 0, In(1));
        scheduler.Submit(1, 0, In(7));
        ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);

        EXPECT_FALSE(scheduler.DropSlot(1, 0)) << "not in the past";
        EXPECT_FALSE(scheduler.DropSlot(5, 1));
        EXPECT_TRUE(scheduler.DropSlot(1, 1));
        EXPECT_FALSE(scheduler.DropSlot(1, 3)) << "once";
        EXPECT_EQ(scheduler.Submit(1, 1, In(2)), SubmitResult::SlotDropped);
        scheduler.Submit(0, 1, In(2));
        ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable) << "the dropped slot no longer gates";
        EXPECT_EQ(out.inputs[1], fill == DropFill::Neutral ? Neutral() : In(7));
        scheduler.Submit(0, 2, In(3));
        ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
        EXPECT_EQ(out.inputs[1], fill == DropFill::Neutral ? Neutral() : In(7));
        EXPECT_EQ(out.inputs[0], In(3));
    }
}

TEST(LockstepScheduler, TicksWrapModulo2To32)
{
    LockstepConfig config = Config(2, 2);
    config.startTick = 0xFFFFFFF0u;
    LockstepScheduler a(config);
    LockstepScheduler b(config);
    TickInputs outA;
    TickInputs outB;
    std::vector<std::uint32_t> ticks;
    for (int step = 0; step < 64; ++step)
    {
        std::uint32_t tickA = 0;
        std::uint32_t tickB = 0;
        const LockstepInput inputA = In(step, 1, static_cast<std::uint32_t>(step));
        const LockstepInput inputB = In(-step, 2, 0x100u + static_cast<std::uint32_t>(step));
        ASSERT_EQ(a.SubmitLocal(0, inputA, tickA), SubmitResult::Accepted);
        ASSERT_EQ(b.SubmitLocal(1, inputB, tickB), SubmitResult::Accepted);
        EXPECT_EQ(tickA, tickB + 0u);
        EXPECT_EQ(a.Submit(1, tickB, inputB), SubmitResult::Accepted);
        EXPECT_EQ(b.Submit(0, tickA, inputA), SubmitResult::Accepted);
        ASSERT_EQ(a.TryAdvance(outA), AdvanceStatus::Runnable);
        ASSERT_EQ(b.TryAdvance(outB), AdvanceStatus::Runnable);
        EXPECT_EQ(outA.tick, outB.tick);
        EXPECT_EQ(outA.inputs[0], outB.inputs[0]);
        EXPECT_EQ(outA.inputs[1], outB.inputs[1]);
        ticks.push_back(outA.tick);
    }
    EXPECT_EQ(ticks.front(), 0xFFFFFFF0u);
    EXPECT_EQ(ticks[15], 0xFFFFFFFFu);
    EXPECT_EQ(ticks[16], 0u) << "wrapped";
    EXPECT_EQ(ticks[17], 1u);
    EXPECT_EQ(a.CurrentTick(), 48u);
    // After the wrap the tick 2 ago is old and a tick 3 ahead is accepted
    EXPECT_EQ(a.Submit(0, 46, In(1)), SubmitResult::TooOld);
    EXPECT_EQ(a.Submit(0, 0xFFFFFFFFu, In(1)), SubmitResult::TooOld);
    // Step s's input is for tick start + 2 + s, so tick 16 (32 past the start, across the wrap) holds step 30's
    ASSERT_TRUE(a.Get(0, 16u).has_value());
    EXPECT_EQ(*a.Get(0, 16u), In(30, 1, 30u));
}

TEST(LockstepScheduler, RingEntriesDoNotAliasAcrossAWindow)
{
    LockstepScheduler scheduler(Config(1, 0));
    TickInputs out;
    for (std::uint32_t tick = 0; tick < kLockstepWindow * 3; ++tick)
    {
        ASSERT_EQ(scheduler.Submit(0, tick, In(static_cast<int>(tick % 100))), SubmitResult::Accepted) << tick;
        ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
        EXPECT_EQ(out.inputs[0], In(static_cast<int>(tick % 100)));
    }
    // A tick that is still held answers; one a window older was overwritten
    EXPECT_TRUE(scheduler.Get(0, kLockstepWindow * 3 - 1).has_value());
    EXPECT_FALSE(scheduler.Get(0, kLockstepWindow * 3 - 1 - kLockstepWindow).has_value());
    EXPECT_FALSE(scheduler.Get(0, kLockstepWindow * 3).has_value());
}

TEST(LockstepScheduler, NextNeededIsTheFirstGap)
{
    LockstepScheduler scheduler(Config(2, 2));
    EXPECT_EQ(scheduler.NextNeeded(1), 2u);
    scheduler.Submit(1, 2, In(1));
    scheduler.Submit(1, 3, In(1));
    scheduler.Submit(1, 5, In(1));
    EXPECT_EQ(scheduler.NextNeeded(1), 4u);
    EXPECT_EQ(scheduler.NextNeeded(9), 0u);
}

// ----------------------------------------------------------------------------------------------------------------------
// The bundle codec
// ----------------------------------------------------------------------------------------------------------------------

TEST(InputBundle, RoundTrips)
{
    InputBundle bundle = MakeBundle(0);
    bundle.slot = 3;
    bundle.ackTick = 0xFFFFFFFEu;
    bundle.firstTick = 0xFFFFFFFFu;
    for (int i = 0; i < 10; ++i)
    {
        bundle.frames.push_back(In(i - 5, 127 - i, 0x20000000u + static_cast<std::uint32_t>(i)));
    }
    std::array<std::uint8_t, 256> buffer{};
    const std::size_t size = EncodeInputBundle(bundle, buffer);
    ASSERT_EQ(size, InputBundleSize(10, 6));
    const auto decoded = DecodeInputBundle(std::span<const std::uint8_t>(buffer.data(), size), Layout());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->slot, 3);
    EXPECT_EQ(decoded->ackTick, 0xFFFFFFFEu);
    EXPECT_EQ(decoded->firstTick, 0xFFFFFFFFu);
    EXPECT_EQ(decoded->frames, bundle.frames);
}

TEST(InputBundle, EncodeRefusesTooManyFramesAndTooSmallBuffers)
{
    InputBundle bundle = MakeBundle(kMaxBundleFrames + 1);
    std::array<std::uint8_t, 1024> big{};
    EXPECT_EQ(EncodeInputBundle(bundle, big), 0u);
    bundle = MakeBundle(4);
    std::array<std::uint8_t, 10> small{};
    EXPECT_EQ(EncodeInputBundle(bundle, small), 0u);
}

TEST(InputBundle, DecodeRefusesHostileBytes)
{
    InputBundle bundle = MakeBundle(3);
    std::array<std::uint8_t, 64> buffer{};
    const std::size_t size = EncodeInputBundle(bundle, buffer);
    ASSERT_GT(size, 0u);
    auto view = [&](const std::size_t length) { return std::span<const std::uint8_t>(buffer.data(), length); };

    EXPECT_TRUE(DecodeInputBundle(view(size), Layout()).has_value());
    EXPECT_FALSE(DecodeInputBundle(view(0), Layout()).has_value());
    for (std::size_t length = 1; length < size; ++length)
    {
        EXPECT_FALSE(DecodeInputBundle(view(length), Layout()).has_value()) << "truncated to " << length;
    }
    buffer[size] = 0;
    EXPECT_FALSE(DecodeInputBundle(view(size + 1), Layout()).has_value()) << "trailing byte";

    std::array<std::uint8_t, 64> wrongKind = buffer;
    wrongKind[0] = 9;
    EXPECT_FALSE(DecodeInputBundle(std::span<const std::uint8_t>(wrongKind.data(), size), Layout()).has_value());

    // A count above the cap, and a count the bytes cannot back, are refused before anything is reserved
    std::array<std::uint8_t, 64> counted = buffer;
    counted[10] = 255;
    EXPECT_FALSE(DecodeInputBundle(std::span<const std::uint8_t>(counted.data(), size), Layout()).has_value());
    counted[10] = static_cast<std::uint8_t>(kMaxBundleFrames);
    EXPECT_FALSE(DecodeInputBundle(std::span<const std::uint8_t>(counted.data(), size), Layout()).has_value());
}

TEST(InputBundle, RandomBytesNeverCrashAndNeverExceedTheCap)
{
    SimRandom random(0xC0FFEE);
    std::array<std::uint8_t, 300> bytes{};
    int accepted = 0;
    for (int round = 0; round < 20000; ++round)
    {
        const std::size_t length = random.NextBelow(static_cast<std::uint32_t>(bytes.size()) + 1);
        for (std::size_t i = 0; i < length; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(random.Next());
        }
        if (length > 0 && random.NextBelow(2) == 0)
        {
            bytes[0] = 1;
        }
        const auto decoded = DecodeInputBundle(std::span<const std::uint8_t>(bytes.data(), length), Layout());
        if (decoded)
        {
            ++accepted;
            EXPECT_LE(decoded->frames.size(), kMaxBundleFrames);
        }
    }
    EXPECT_GE(accepted, 0);
}

TEST(InputBundle, BuildBundleForSendsWhatThePeerLacks)
{
    LockstepScheduler scheduler(Config(2, 2));
    for (std::uint32_t tick = 2; tick < 12; ++tick)
    {
        scheduler.Submit(0, tick, In(static_cast<int>(tick)));
    }
    const InputBundle all = BuildBundleFor(scheduler, 0, 0);
    EXPECT_EQ(all.firstTick, 0u);
    EXPECT_EQ(all.frames.size(), 12u);
    EXPECT_EQ(all.frames[5], In(5));
    const InputBundle some = BuildBundleFor(scheduler, 0, 8);
    EXPECT_EQ(some.firstTick, 8u);
    EXPECT_EQ(some.frames.size(), 4u);
    EXPECT_TRUE(BuildBundleFor(scheduler, 0, 12).frames.empty());
    EXPECT_TRUE(BuildBundleFor(scheduler, 0, 500).frames.empty());
    EXPECT_TRUE(BuildBundleFor(scheduler, 7, 0).frames.empty());
    for (std::uint32_t tick = 12; tick < 100; ++tick)
    {
        scheduler.Submit(0, tick, In(1));
    }
    EXPECT_EQ(BuildBundleFor(scheduler, 0, 0).frames.size(), kMaxBundleFrames) << "capped";
}

// ----------------------------------------------------------------------------------------------------------------------
// Three peers over impaired SimTransport links
// ----------------------------------------------------------------------------------------------------------------------

namespace
{
    struct Peer
    {
        Peer(const LockstepConfig &config, const std::uint32_t slot, SimTransport *transport)
            : scheduler(config), slot(slot), transport(transport)
        {
        }

        LockstepScheduler scheduler;
        std::uint32_t slot;
        SimTransport *transport;
        std::array<std::uint32_t, kMaxLockstepSlots> peerAck{};
        std::vector<TickInputs> ran;
        std::uint32_t localCount = 0;
    };

    LockstepInput InputFor(const std::uint32_t slot, const std::uint32_t tick)
    {
        // A pure function of (slot, tick): what the player "did"
        return In(static_cast<int>((tick * 7 + slot * 13) % 255) - 127, static_cast<int>((tick * 3 + slot) % 255) - 127,
                  tick * 2654435761u + slot);
    }

    // Runs `peers` peers, fully meshed, until each has run `target` ticks or `maxFrames` frames passed
    std::vector<std::vector<TickInputs>> RunMesh(const std::uint64_t seed, const SimLinkConfig &link, const std::uint32_t peers,
                                                 const std::uint32_t target, const std::uint32_t startTick, const int maxFrames,
                                                 int *framesUsed = nullptr)
    {
        SimNetwork network(seed);
        std::vector<Peer> all;
        std::vector<SimTransport *> transports;
        for (std::uint32_t i = 0; i < peers; ++i)
        {
            transports.push_back(&network.AddEndpoint());
        }
        for (std::uint32_t i = 0; i < peers; ++i)
        {
            for (std::uint32_t j = i + 1; j < peers; ++j)
            {
                EXPECT_TRUE(network.Link(transports[i]->Id(), transports[j]->Id(), link));
            }
        }
        LockstepConfig config = Config(peers, 2);
        config.startTick = startTick;
        for (std::uint32_t i = 0; i < peers; ++i)
        {
            all.emplace_back(config, i, transports[i]);
            for (auto &ack : all.back().peerAck)
            {
                ack = startTick;
            }
        }

        int frame = 0;
        for (; frame < maxFrames; ++frame)
        {
            network.Advance(16'000);
            bool done = true;
            for (Peer &peer : all)
            {
                peer.transport->Poll(
                    [&](const TransportEvent &event)
                    {
                        if (event.type != TransportEventType::Datagram)
                        {
                            return;
                        }
                        const auto bundle = DecodeInputBundle(event.data, Layout());
                        if (!bundle || bundle->slot >= peers)
                        {
                            return;
                        }
                        // The sender's slot is its transport id - 1 here; a bundle for any other slot is not accepted
                        if (bundle->slot + 1u != event.peer)
                        {
                            return;
                        }
                        peer.peerAck[bundle->slot] = bundle->ackTick;
                        std::uint32_t tick = bundle->firstTick;
                        for (const LockstepInput &input : bundle->frames)
                        {
                            (void)peer.scheduler.Submit(bundle->slot, tick, input);
                            ++tick;
                        }
                    });
                // One local input per frame, for the tick the scheduler will assign it (the slot's next local tick)
                std::uint32_t assigned = 0;
                const std::uint32_t wanted = startTick + 2 + peer.localCount;
                if (peer.scheduler.SubmitLocal(peer.slot, InputFor(peer.slot, wanted), assigned) == SubmitResult::Accepted)
                {
                    EXPECT_EQ(assigned, wanted);
                    ++peer.localCount;
                }
                TickInputs out;
                while (peer.ran.size() < target && peer.scheduler.TryAdvance(out) == AdvanceStatus::Runnable)
                {
                    peer.ran.push_back(out);
                }
                std::array<std::uint8_t, 256> buffer{};
                for (std::uint32_t other = 0; other < peers; ++other)
                {
                    if (other == peer.slot)
                    {
                        continue;
                    }
                    const InputBundle bundle = BuildBundleFor(peer.scheduler, peer.slot, peer.peerAck[other]);
                    const std::size_t size = EncodeInputBundle(bundle, buffer);
                    if (size > 0)
                    {
                        (void)peer.transport->Send(transports[other]->Id(), std::span<const std::uint8_t>(buffer.data(), size));
                    }
                }
                done = done && peer.ran.size() >= target;
            }
            if (done)
            {
                break;
            }
        }
        if (framesUsed != nullptr)
        {
            *framesUsed = frame;
        }
        std::vector<std::vector<TickInputs>> result;
        for (Peer &peer : all)
        {
            result.push_back(peer.ran);
        }
        return result;
    }

    // Every peer ran the same ticks with the same inputs, and the inputs are the ones the players produced
    void ExpectAgreement(const std::vector<std::vector<TickInputs>> &runs, const std::uint32_t target, const std::uint32_t startTick)
    {
        ASSERT_FALSE(runs.empty());
        for (const auto &run : runs)
        {
            ASSERT_EQ(run.size(), target) << "a peer did not finish";
        }
        for (std::uint32_t i = 0; i < target; ++i)
        {
            const std::uint32_t tick = startTick + i;
            for (std::size_t peer = 0; peer < runs.size(); ++peer)
            {
                ASSERT_EQ(runs[peer][i].tick, tick);
                for (std::uint32_t slot = 0; slot < runs.size(); ++slot)
                {
                    const LockstepInput expected = i < 2 ? Neutral() : InputFor(slot, tick);
                    ASSERT_EQ(runs[peer][i].inputs[slot], expected) << "peer " << peer << " slot " << slot << " tick " << i;
                }
            }
        }
    }
}

TEST(LockstepMesh, PerfectLinksRunInLockstepAtOneTickPerFrame)
{
    SimLinkConfig link;
    link.latencyMicros = 10'000;
    int frames = 0;
    const auto runs = RunMesh(1, link, 3, 200, 0, 400, &frames);
    ExpectAgreement(runs, 200, 0);
    EXPECT_LT(frames, 260) << "a 10 ms link and 2 ticks of delay should not stall a 16 ms tick for long";
}

TEST(LockstepMesh, SurvivesLossReorderDuplicationAndJitter)
{
    SimLinkConfig link;
    link.latencyMicros = 30'000;
    link.jitterMicros = 40'000;
    link.lossPpm = 250'000;
    link.burstEnterPpm = 20'000;
    link.burstExitPpm = 200'000;
    link.burstLossPpm = 900'000;
    link.reorderPpm = 150'000;
    link.reorderDelayMicros = 60'000;
    link.duplicatePpm = 150'000;
    for (const std::uint64_t seed : {7ull, 8ull, 9ull})
    {
        const auto runs = RunMesh(seed, link, 3, 300, 0, 6000);
        ExpectAgreement(runs, 300, 0);
    }
}

TEST(LockstepMesh, CrossesTheTickWrapUnderLoss)
{
    SimLinkConfig link;
    link.latencyMicros = 20'000;
    link.jitterMicros = 20'000;
    link.lossPpm = 200'000;
    link.duplicatePpm = 100'000;
    const std::uint32_t start = 0xFFFFFFC0u;
    const auto runs = RunMesh(21, link, 2, 200, start, 4000);
    ExpectAgreement(runs, 200, start);
}

TEST(LockstepMesh, SameSeedSameRun)
{
    SimLinkConfig link;
    link.latencyMicros = 25'000;
    link.jitterMicros = 30'000;
    link.lossPpm = 300'000;
    int framesA = 0;
    int framesB = 0;
    const auto a = RunMesh(99, link, 3, 150, 0, 6000, &framesA);
    const auto b = RunMesh(99, link, 3, 150, 0, 6000, &framesB);
    EXPECT_EQ(framesA, framesB) << "the stall pattern is part of the determinism";
    ExpectAgreement(a, 150, 0);
}

TEST(LockstepMesh, TotalLossStallsInsteadOfInventingInputs)
{
    SimLinkConfig link;
    link.lossPpm = 1'000'000;
    const auto runs = RunMesh(5, link, 2, 100, 0, 300);
    for (const auto &run : runs)
    {
        // Ticks 0 and 1 are the delay prefill; nothing else can run without the other player's input
        EXPECT_EQ(run.size(), 2u);
    }
}
