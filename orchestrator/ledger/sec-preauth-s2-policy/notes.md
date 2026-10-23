Accepted by the user 2026-10-13:
- Loopback has no per-address handshake bucket in TokenBucket (only the global one), mirroring its no per-address slot cap; the design text did not say.
- With the PreAuthSlots reserve option on, loopback may use only the open slots (the reserve is for recently authenticated non-loopback addresses); option stays OFF by default.
- Peers of unknown address family share one address (and its per-address cap) in PreAuthSlots and TokenBucket.
- SessionArbiter: no-token rule is a constructor flag/parameter; Begin records the candidate and reports the replaced session id; End(id) ignores stale ids.
- A wrong token during a user's backoff keeps answering RateLimitedError (not 'Invalid access token') but is now counted via RecordFailure (user, address, loopback) and logged like any backoff start.
- Existing test UserRateLimitTest.FailuresNamingAUserBackThatUserOffFromAnywhereAndOthersAreServed changed: it asserted the old order (right token refused during backoff); now wrong tokens are refused, the right token gets in, log count of user=alice lines is 3.
- RateLimiter reads the injected clock before taking its mutex (no call out under lock); comments in RateLimiter.hpp and EditorServer.hpp about 'no lock' updated.
- No TSan option exists in the repo, so thread safety is covered by stress tests with exact-count assertions only.
- Review fixes: per-user backoff Warn text now says a Hello naming the user is still checked (tests match the prefix only); docs no longer claim the per-user rule limits guessing.
