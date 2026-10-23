Follow-ups of #211, J4 (#213) and J5 (#212): the LOW review findings the user accepted.

- J4: a trigger contact that is only inside Jolt's speculative distance no longer counts as present in FoldContacts, so a tracked trigger touch under an old key ends with an Exit instead of being taken over by a key that never reports it. Test: a trigger approached slowly, crossed and left gives one Enter and one Exit.
- J4 docs: the listener sentence and the re-Enter after SetIsTrigger / collision-matrix change now match the code.
- J5: RaycastAll uses the single-hit pick repeatedly (kQueryTieTolerance, then handle, then collider) on both backends. Test: near-equal hits come in the single-hit order.
- J5 PhysX: single-hit queries run one touching query (blocking + bounded second query only when the 1024-entry buffer fills); the hit buffer is a per-thread scratch instead of a heap allocation per query.
- #211: a build with no backend at all logs a Warning instead of an Error (Deterministic refusals and every other refusal stay Errors).
- docs/index.html: row for design-jolt-queries.

Not verified: nothing builds locally; CI is the build. The PhysX single-hit change relies on a non-full touch buffer holding every hit (PxHitBuffer touches), not run locally.
