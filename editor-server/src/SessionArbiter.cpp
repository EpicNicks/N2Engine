#include "editor-server/SessionArbiter.hpp"

namespace N2Engine::Editor
{
    const char *ArbiterDecisionName(const ArbiterDecision decision)
    {
        switch (decision)
        {
        case ArbiterDecision::Accept:
            return "Accept";
        case ArbiterDecision::Takeover:
            return "Takeover";
        case ArbiterDecision::Preempt:
            return "Preempt";
        case ArbiterDecision::Busy:
            return "Busy";
        }
        return "Busy";
    }

    ArbiterVerdict SessionArbiter::Decide(const std::optional<SessionIdentity> &active, const SessionIdentity &candidate,
                                          const bool noTokenHost)
    {
        if (!active.has_value())
        {
            return {ArbiterDecision::Accept, "no session is active"};
        }
        if (noTokenHost)
        {
            return {ArbiterDecision::Takeover, "a host without a token: the newest client takes over"};
        }
        if (active->user == candidate.user && active->role == candidate.role)
        {
            return {ArbiterDecision::Takeover, "the same user and role: the newest session takes over"};
        }
        // TokenRole is declared in rank order: Viewer < Editor < Admin
        if (static_cast<uint8_t>(candidate.role) > static_cast<uint8_t>(active->role))
        {
            return {ArbiterDecision::Preempt, "the candidate's role is higher than the active session's"};
        }
        return {ArbiterDecision::Busy, "the host is serving another session"};
    }

    ArbiterVerdict SessionArbiter::Evaluate(const SessionIdentity &candidate) const
    {
        const std::lock_guard lock(_mutex);
        return Decide(_active, candidate, _noTokenHost);
    }

    SessionArbiter::BeginResult SessionArbiter::Begin(const SessionIdentity &candidate)
    {
        const std::lock_guard lock(_mutex);
        BeginResult result;
        result.verdict = Decide(_active, candidate, _noTokenHost);
        if (result.verdict.decision == ArbiterDecision::Busy)
        {
            return result;
        }
        result.replacedSessionId = _active.has_value() ? _activeId : 0;
        // 0 means "none", so the counter skips it on wrap
        ++_lastId;
        if (_lastId == 0)
        {
            ++_lastId;
        }
        _activeId = _lastId;
        _active = candidate;
        result.sessionId = _activeId;
        return result;
    }

    void SessionArbiter::End(const uint64_t sessionId)
    {
        const std::lock_guard lock(_mutex);
        if (_active.has_value() && sessionId != 0 && sessionId == _activeId)
        {
            _active.reset();
            _activeId = 0;
        }
    }

    void SessionArbiter::UpdateIdentity(const uint64_t sessionId, const SessionIdentity &identity)
    {
        const std::lock_guard lock(_mutex);
        if (_active.has_value() && sessionId != 0 && sessionId == _activeId)
        {
            _active = identity;
        }
    }

    std::optional<SessionIdentity> SessionArbiter::Active() const
    {
        const std::lock_guard lock(_mutex);
        return _active;
    }

    uint64_t SessionArbiter::ActiveSessionId() const
    {
        const std::lock_guard lock(_mutex);
        return _active.has_value() ? _activeId : 0;
    }
}
