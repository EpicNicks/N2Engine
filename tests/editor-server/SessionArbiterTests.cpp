#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <editor-server/SessionArbiter.hpp>

using namespace N2Engine::Editor;

namespace
{
    constexpr std::array<TokenRole, 3> Roles = {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin};

    SessionIdentity Who(const std::string &user, const TokenRole role)
    {
        return SessionIdentity{user, role};
    }

    int Rank(const TokenRole role)
    {
        return role == TokenRole::Viewer ? 0 : role == TokenRole::Editor ? 1 : 2;
    }
}

TEST(SessionArbiterTest, NoActiveSessionAcceptsAnyone)
{
    for (const TokenRole role : Roles)
    {
        const ArbiterVerdict verdict = SessionArbiter::Decide(std::nullopt, Who("alice", role));
        EXPECT_EQ(verdict.decision, ArbiterDecision::Accept);
        EXPECT_NE(std::string(verdict.reason), "");
    }
}

TEST(SessionArbiterTest, FullRoleMatrixForTheSameUser)
{
    for (const TokenRole active : Roles)
    {
        for (const TokenRole candidate : Roles)
        {
            const ArbiterVerdict verdict = SessionArbiter::Decide(Who("alice", active), Who("alice", candidate));
            ArbiterDecision expected = ArbiterDecision::Busy;
            if (active == candidate)
                expected = ArbiterDecision::Takeover;
            else if (Rank(candidate) > Rank(active))
                expected = ArbiterDecision::Preempt;
            EXPECT_EQ(verdict.decision, expected)
                << "active " << Rank(active) << " candidate " << Rank(candidate) << ": " << verdict.reason;
        }
    }
}

TEST(SessionArbiterTest, FullRoleMatrixForAnotherUser)
{
    for (const TokenRole active : Roles)
    {
        for (const TokenRole candidate : Roles)
        {
            const ArbiterVerdict verdict = SessionArbiter::Decide(Who("alice", active), Who("bob", candidate));
            // Equal roles of two users do not take over from each other: only strictly higher preempts
            const ArbiterDecision expected =
                Rank(candidate) > Rank(active) ? ArbiterDecision::Preempt : ArbiterDecision::Busy;
            EXPECT_EQ(verdict.decision, expected)
                << "active " << Rank(active) << " candidate " << Rank(candidate) << ": " << verdict.reason;
        }
    }
}

TEST(SessionArbiterTest, NamedCases)
{
    // A viewer holding the host does not lock an admin out (F2)
    EXPECT_EQ(SessionArbiter::Decide(Who("v", TokenRole::Viewer), Who("a", TokenRole::Admin)).decision,
              ArbiterDecision::Preempt);
    // An editor does not preempt an editor
    EXPECT_EQ(SessionArbiter::Decide(Who("e1", TokenRole::Editor), Who("e2", TokenRole::Editor)).decision,
              ArbiterDecision::Busy);
    // The same user reconnecting after a crash takes over
    EXPECT_EQ(SessionArbiter::Decide(Who("e1", TokenRole::Editor), Who("e1", TokenRole::Editor)).decision,
              ArbiterDecision::Takeover);
    // An admin is never preempted, and a lower role of the same user does not displace it
    EXPECT_EQ(SessionArbiter::Decide(Who("a", TokenRole::Admin), Who("a", TokenRole::Viewer)).decision,
              ArbiterDecision::Busy);
    EXPECT_EQ(SessionArbiter::Decide(Who("a", TokenRole::Admin), Who("b", TokenRole::Admin)).decision,
              ArbiterDecision::Busy);
}

TEST(SessionArbiterTest, UserNamesAreCaseSensitiveAsInTheTokensFile)
{
    EXPECT_EQ(SessionArbiter::Decide(Who("Alice", TokenRole::Editor), Who("alice", TokenRole::Editor)).decision,
              ArbiterDecision::Busy);
}

TEST(SessionArbiterTest, ANoTokenHostLetsTheNewestClientTakeOver)
{
    // Every client is an admin with an empty user
    const SessionIdentity anonymous = Who("", TokenRole::Admin);
    EXPECT_EQ(SessionArbiter::Decide(anonymous, anonymous, true).decision, ArbiterDecision::Takeover);
    EXPECT_EQ(SessionArbiter::Decide(std::nullopt, anonymous, true).decision, ArbiterDecision::Accept);
    // The flag decides, whatever the identities (a host without tokens has no other identities)
    EXPECT_EQ(SessionArbiter::Decide(Who("", TokenRole::Admin), Who("x", TokenRole::Viewer), true).decision,
              ArbiterDecision::Takeover);

    // The same follows without the flag for a host where everybody shares one identity
    EXPECT_EQ(SessionArbiter::Decide(anonymous, anonymous, false).decision, ArbiterDecision::Takeover);

    SessionArbiter arbiter(true);
    EXPECT_TRUE(arbiter.NoTokenHost());
    const SessionArbiter::BeginResult first = arbiter.Begin(anonymous);
    EXPECT_EQ(first.verdict.decision, ArbiterDecision::Accept);
    const SessionArbiter::BeginResult second = arbiter.Begin(anonymous);
    EXPECT_EQ(second.verdict.decision, ArbiterDecision::Takeover);
    EXPECT_EQ(second.replacedSessionId, first.sessionId);
}

TEST(SessionArbiterTest, EveryDecisionHasAName)
{
    EXPECT_STREQ(ArbiterDecisionName(ArbiterDecision::Accept), "Accept");
    EXPECT_STREQ(ArbiterDecisionName(ArbiterDecision::Takeover), "Takeover");
    EXPECT_STREQ(ArbiterDecisionName(ArbiterDecision::Preempt), "Preempt");
    EXPECT_STREQ(ArbiterDecisionName(ArbiterDecision::Busy), "Busy");
}

TEST(SessionArbiterTest, TheHolderBeginsAndEndsSessions)
{
    SessionArbiter arbiter;
    EXPECT_FALSE(arbiter.Active().has_value());
    EXPECT_EQ(arbiter.ActiveSessionId(), 0u);

    const SessionArbiter::BeginResult viewer = arbiter.Begin(Who("v", TokenRole::Viewer));
    EXPECT_EQ(viewer.verdict.decision, ArbiterDecision::Accept);
    EXPECT_NE(viewer.sessionId, 0u);
    EXPECT_EQ(viewer.replacedSessionId, 0u);
    ASSERT_TRUE(arbiter.Active().has_value());
    EXPECT_EQ(arbiter.Active()->user, "v");

    // An editor preempts the viewer: the viewer's id is reported so the caller closes it
    const SessionArbiter::BeginResult editor = arbiter.Begin(Who("e", TokenRole::Editor));
    EXPECT_EQ(editor.verdict.decision, ArbiterDecision::Preempt);
    EXPECT_EQ(editor.replacedSessionId, viewer.sessionId);
    EXPECT_NE(editor.sessionId, viewer.sessionId);
    EXPECT_EQ(arbiter.ActiveSessionId(), editor.sessionId);

    // The replaced session ending later must not clear its successor
    arbiter.End(viewer.sessionId);
    ASSERT_TRUE(arbiter.Active().has_value());
    EXPECT_EQ(arbiter.Active()->user, "e");

    // Busy records nothing
    const SessionArbiter::BeginResult busy = arbiter.Begin(Who("v2", TokenRole::Viewer));
    EXPECT_EQ(busy.verdict.decision, ArbiterDecision::Busy);
    EXPECT_EQ(busy.sessionId, 0u);
    EXPECT_EQ(busy.replacedSessionId, 0u);
    EXPECT_EQ(arbiter.ActiveSessionId(), editor.sessionId);

    // Evaluate is read only
    EXPECT_EQ(arbiter.Evaluate(Who("a", TokenRole::Admin)).decision, ArbiterDecision::Preempt);
    EXPECT_EQ(arbiter.ActiveSessionId(), editor.sessionId);

    // End of the active one, and a stale or zero id
    arbiter.End(0);
    EXPECT_TRUE(arbiter.Active().has_value());
    arbiter.End(editor.sessionId);
    EXPECT_FALSE(arbiter.Active().has_value());
    arbiter.End(editor.sessionId); // twice is harmless
    EXPECT_EQ(arbiter.Evaluate(Who("v2", TokenRole::Viewer)).decision, ArbiterDecision::Accept);
}

TEST(SessionArbiterTest, SessionIdsNeverRepeatWhileNothingEnds)
{
    SessionArbiter arbiter;
    uint64_t previous = 0;
    for (int i = 0; i < 100; ++i)
    {
        const SessionArbiter::BeginResult result = arbiter.Begin(Who("same", TokenRole::Editor));
        EXPECT_GT(result.sessionId, previous);
        previous = result.sessionId;
    }
}

TEST(SessionArbiterTest, ConcurrentBeginsLeaveExactlyOneActiveSession)
{
    SessionArbiter arbiter;
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back(
            [&arbiter, &accepted, t]
            {
                for (int i = 0; i < 500; ++i)
                {
                    // Equal roles of different users: only the first Begin can win
                    const SessionArbiter::BeginResult result =
                        arbiter.Begin(Who("user" + std::to_string(t), TokenRole::Editor));
                    if (result.sessionId != 0)
                    {
                        ++accepted;
                    }
                    (void)arbiter.Evaluate(Who("other", TokenRole::Viewer));
                    (void)arbiter.Active();
                }
            });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    // The first thread to arrive holds the session; its own later Begins are Takeovers, the others' are Busy. Whoever
    // won, the holder still has one coherent session.
    ASSERT_TRUE(arbiter.Active().has_value());
    EXPECT_NE(arbiter.ActiveSessionId(), 0u);
    EXPECT_GE(accepted.load(), 500) << "the winner's own 500 Begins are all Takeovers";
}
