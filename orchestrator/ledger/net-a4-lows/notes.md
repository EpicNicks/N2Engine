Accepted by the user 2026-10-13:
- The A4 ledger pr-body.md named in the brief does not exist; the accepted Disconnect wording was taken from the merged PR #203 body instead
- Idle sweep judges against the Poll's start time with a last<=now guard (no unsigned underflow), so peers heard during the drain are never expired in that Poll
- An IPv4-mapped IPv6 bind (::ffff:127.0.0.1) is no longer reported dual-stack, so an IPv4 Connect on it now returns FamilyMismatch (follows 'DualStack only for ::' literally; untested)
- Stall test uses one deliberate fixed 900 ms stall (the condition under test); docs/testing.html updated to say so
