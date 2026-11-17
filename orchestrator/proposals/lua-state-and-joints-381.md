# #381 items 5 and 6: Lua state outside replicated fields, and joints

Assessed against master after PR #380. Both items are design work, not small fixes, so this is a proposal and no engine code changed.

## Item 5: Lua state outside declared replicated fields

**What exists.** Only fields a script declares `replicated = true` are in a rollback snapshot (`RollbackWorld`). `RollbackRuntime::LuaStateWarnings` (engine/src/network/RollbackRuntime.cpp) runs once per session over every `LuaComponent` and logs the common cases via `LuaComponent::FindTickStateOutsideSnapshot` (module locals, non-replicated fields written in a tick, globals, module tables, helpers reached through local functions and `self:` calls, every self write on an object without a `NetworkObject`). It is tested in `RollbackLuaTest.TheSessionLint*`. docs/netcode.html "Limits that remain" documents the rest: locals, closures, coroutines and interpreter state. The desync detector is the backstop.

**What is left.** Only the inherent gap: a lint is static and incomplete, so a script can still diverge and is caught only when the hash disagrees. Nothing in item 5 is a bug to fix in one PR.

**Options**
1. *Keep as is* (lint plus detector). Cost: none. Gap: silent until a hash interval fails.
2. *Restricted sim-Lua profile* (the "later" in physx-determinism-157.md row 12): under a Deterministic preset, scripts' tick callbacks run with `setmetatable`-guarded `self`, no `coroutine` library, no `load`/`require` of new state, and writes to non-replicated fields or upvalues raise an error instead of a warning. Medium cost (sandbox in `LuaDeterministic.cpp`, per-callback guard), breaks scripts that keep caches. Gives a hard guarantee for the covered cases; closures over upvalues need a bytecode or debug-hook check and stay approximate.
3. *Snapshot the Lua state itself* (serialise the script table graph). High cost and fragile (functions, userdata, coroutines are not serialisable); not recommended.

**Recommendation:** option 1 now; option 2 as a follow-up only when a game needs the guarantee. Decision for the owner.

## Item 6: joint component

**What exists.** Nothing: `IPhysicsBackend` has no joint call; Jolt snapshots say "constraints (the backend uses none)"; PhysX is used raw only in `PhysXRagdollDeterminismTests.cpp`.

**Proposed shape** (needs a stated game need first, per custom-physics-156.md decision C/R6)
- Seam: `IPhysicsBackend` gains `CreateJoint(const JointDesc&) -> PhysicsJointHandle`, `DestroyJoint`, `SetJointLimits`, `SetJointMotor`, defaulted to "unsupported" and a `PhysicsFeatureSet` bit `Joints` so `ValidateNetwork` keeps requiring the full set only for backends that claim it.
- `JointDesc`: type (Fixed, Hinge, Ball, Slider, Distance first), two body handles (the second optional = world anchor), local anchors and axes, limits, breakable force, collide-connected flag.
- Component: `Joint` (engine/src/physics) with `connectedBody` reference, serialised like other components, created on attach once both bodies exist, destroyed with either body.
- Jolt: `TwoBodyConstraint` subclasses (Fixed, Hinge, Point, Slider, Distance). PhysX: `PxFixedJointCreate`, `PxRevoluteJointCreate`, `PxSphericalJointCreate`, `PxPrismaticJointCreate`, `PxDistanceJointCreate`.
- Rollback: Jolt `SaveState`/`RestoreState` must include constraint state (accumulated impulses for warm starting) and the joint set; joint creation order must be deterministic (by net id). `HashSimulatedState` must cover them. PhysX stays authoritative-only.
- Tests: per backend create/destroy/limit/motor behaviour; Jolt save-restore-resume equals straight run with a ragdoll scene; add the ragdoll to the determinism corpus (item 7).
- Box2D (2D) has joints upstream but the 2D world is not in rollback, so 2D joints are out of scope.

**Cost:** L (several PRs): seam and Jolt first, then PhysX, then snapshot/hash, then editor inspector. Do not start without a named game need.
