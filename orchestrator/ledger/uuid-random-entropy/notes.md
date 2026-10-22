Accepted by the user 2026-10-14:
- Chose seeded thread_local mt19937_64 over getrandom/BCryptGenRandom (simpler, portable)
- Also added a UUIDRandomTests row to docs/testing.html
- Started tests.yml via workflow_dispatch run 38626523971 in addition to the PR-triggered runs
Branch updated to master after #138; needs CI + a fresh re-confirm review of the new head, then ratify --merge.
