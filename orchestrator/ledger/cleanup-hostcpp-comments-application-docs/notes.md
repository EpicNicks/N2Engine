Accepted by the user 2026-10-13:
- Init reads FOUR project settings (also physics.deterministic), not three as the brief said; docs list and sentence cover all four
- Also corrected stale comments in EditorServer.hpp, EditorServer.cpp (Hello gate), Connection.hpp, PreAuthSlots.hpp, TokenBucket.hpp (proxy peers loopback only over loopback)
- Left Connection.cpp 'serving thread' comment as is (waits run on workers and session)
- Widening: docs/logging-and-editor.html --project row now lists the four settings Init reads
- Widening: --help text for --no-rate-limit and --preauth-slots in HostOptions.cpp corrected (raw-string text only); no test compares those strings
- Also corrected the same proxy-is-loopback claim in RateLimiter.hpp and RateLimiter.cpp comments; tests/ comments left alone (outside allowed paths)
