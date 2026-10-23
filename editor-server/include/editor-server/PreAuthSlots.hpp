#pragma once

// The pre-authentication slots now live in n2security (engine/security/PreAuthSlots.hpp), shared with the netcode's
// handshake. This header keeps the editor host's names: the classes are the same, not copies.
#include "editor-server/RateLimiter.hpp"
#include "engine/security/PreAuthSlots.hpp"

namespace N2Engine::Editor
{
    using Security::PreAuthSlots;
    using Security::PreAuthSlotsOptions;
}
