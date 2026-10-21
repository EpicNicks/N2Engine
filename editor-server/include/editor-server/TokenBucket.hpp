#pragma once

// The handshake rate limit now lives in n2security (engine/security/TokenBucket.hpp), shared with the netcode's
// handshake. This header keeps the editor host's names: the classes are the same, not copies.
#include "editor-server/RateLimiter.hpp"
#include "engine/security/TokenBucket.hpp"

namespace N2Engine::Editor
{
    using Security::TokenBucket;
    using Security::TokenBucketOptions;
}
