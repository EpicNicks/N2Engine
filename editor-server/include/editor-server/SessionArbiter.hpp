#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "editor-server/TokenTable.hpp"

namespace N2Engine::Editor
{
    /**
     * Who gets the one authenticated session when a second verified client arrives (design: security-design Part 2,
     * "Arbiter"). Plain logic, no sockets and no clock. EditorServer's OfferToSession applies it (PR S-4).
     *
     * Given the active session (none, or a user and a role) and a verified candidate:
     * In this order:
     *  - no active session: Accept (a yielding candidate too);
     *  - the candidate yields (Hello `yield`, since 1.22.0): Busy, never Takeover or Preempt, even from the same user and
     *    even on a noTokenHost;
     *  - the active session yields and the candidate does not: Preempt, whatever the roles, the same user and a
     *    noTokenHost included;
     *  - a host without any token (`noTokenHost`): Takeover (every client is an admin with an empty user: the newest one
     *    takes over);
     *  - same user and same role: Takeover (the newest connection of the same user replaces the old one);
     *  - the candidate's role is strictly higher (admin > editor > viewer): Preempt;
     *  - otherwise: Busy.
     * Whole-struct equality is not used: `yield` is judged by its own rules above.
     *
     * Decide is a pure function; SessionArbiter adds a mutex-guarded holder of the active session (Begin/End) with no
     * call out while the lock is held. The decision names what should happen to the old session; closing it and waiting
     * for it to end is the caller's job.
     */
    struct SessionIdentity
    {
        /// Empty on a host without named users
        std::string user;
        TokenRole role = TokenRole::Viewer;
        /// The client asked to yield to people (Hello `yield`, since 1.22.0)
        bool yield = false;

        [[nodiscard]] bool operator==(const SessionIdentity &other) const = default;
    };

    enum class ArbiterDecision : uint8_t
    {
        /// Nobody is active: the candidate becomes the session
        Accept,
        /// Same user and role: the active session is closed and the candidate replaces it
        Takeover,
        /// A strictly higher role: the active session is closed and the candidate replaces it
        Preempt,
        /// The candidate is turned away; the active session is untouched
        Busy,
    };

    struct ArbiterVerdict
    {
        ArbiterDecision decision = ArbiterDecision::Busy;
        /// A fixed sentence for logs (never client text)
        const char *reason = "";
    };

    /// Name of a decision, for logs and tests
    [[nodiscard]] const char *ArbiterDecisionName(ArbiterDecision decision);

    class SessionArbiter
    {
    public:
        /// The pure rule
        [[nodiscard]] static ArbiterVerdict Decide(const std::optional<SessionIdentity> &active,
                                                   const SessionIdentity &candidate, bool noTokenHost = false);

        struct BeginResult
        {
            ArbiterVerdict verdict;
            /// The candidate's session id (never 0); 0 when it was turned away (Busy)
            uint64_t sessionId = 0;
            /// The session replaced (Takeover, Preempt): the caller closes it; 0 when none
            uint64_t replacedSessionId = 0;
        };

        explicit SessionArbiter(bool noTokenHost = false) : _noTokenHost(noTokenHost) {}

        [[nodiscard]] bool NoTokenHost() const { return _noTokenHost; }

        /// What would happen now (read only)
        [[nodiscard]] ArbiterVerdict Evaluate(const SessionIdentity &candidate) const;
        /// Decides and, unless Busy, records the candidate as the active session in one step. A replaced session's later
        /// End(id) does nothing, so it cannot clear its successor.
        [[nodiscard]] BeginResult Begin(const SessionIdentity &candidate);
        /// The session with this id ended. Ignored when it is not the active one (it was replaced, or ended already).
        void End(uint64_t sessionId);
        /// A later Hello on the active session's connection gave it another user or role: the arbiter judges the next
        /// candidate against what the session is now. Ignored when `sessionId` is not the active one.
        void UpdateIdentity(uint64_t sessionId, const SessionIdentity &identity);

        [[nodiscard]] std::optional<SessionIdentity> Active() const;
        [[nodiscard]] uint64_t ActiveSessionId() const;

    private:
        const bool _noTokenHost;
        mutable std::mutex _mutex;
        std::optional<SessionIdentity> _active;
        uint64_t _activeId = 0;
        uint64_t _lastId = 0;
    };
}
