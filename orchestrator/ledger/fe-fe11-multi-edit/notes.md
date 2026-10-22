Accepted by the user 2026-10-13:
- Most of FE11 (MultiInspectorState, mixed display, fan-out in one group, Lua same-script rule) already shipped with FE5; this PR adds Add/Remove for all, abort-on-refusal, stale-selection handling, tests and README
- Failure semantics changed from FE5's continue-past-refusal to abort at the first refusal (per brief); no client-side rollback: the group ends in a finally and Undo takes back what was applied, as the existing edit groups do
- Add Component and Remove for all use the same 'Edit N objects' group label
- A singleton type that ANY selected object has is disabled in the multi Add Component menu
- Existing README multi-edit bullet edited (one sentence) so it does not contradict the new section
- One unrelated Node test (project-session 'shutdown while the old host is still dying') failed once locally, passed on rerun and on a full second npm test run
