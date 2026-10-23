#pragma once

// The failed-Hello rate limiter and the peer address now live in n2security (engine/security/RateLimiter.hpp), shared
// with the netcode's handshake. This header keeps the editor host's names: the classes are the same, not copies.
#include "engine/security/RateLimiter.hpp"

namespace N2Engine::Editor
{
    using Security::PeerAddress;
    using Security::RateLimitPolicy;
    using Security::RateLimiter;
}
