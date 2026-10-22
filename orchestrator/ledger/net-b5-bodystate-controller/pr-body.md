Netcode B5: the engine-owned character controller over the backends' capsule casts, `BodyState` tests on both backends, and the Lua API. PhysX and Jolt behaviour of existing features is unchanged.

## What changed
- `CharacterMotor::Step(backend, settings, state, input, dt)` is the movement step as a pure function of its arguments and the scene's capsule casts and overlaps: collide-and-slide (4 slides at most), ground detection with a slope limit, step up (a raise, a forward move of at least one radius, a drop) and step down, jump and gravity integration, a skin width, penetration recovery bounded at 16 rings by 10 directions. Plain float arithmetic and `Math::Cos` only (no `Vector3` dot, length or normalise, which dispatch on the CPU).
- `CharacterController` component: an upright capsule that moves the object once per fixed tick. Held input (`move`, one-press `jump`), `CharacterState` (position, velocity, grounded, ground normal) that can be copied, written back (`SetState`, what a server correction does) and hashed (`CharacterMotor::Hash`). Settings are serialised and shown in the inspector.
- Lua: `go:AddComponent("CharacterController")` with `SetInput`, `GetInput`, `GetState`, `SetState`, `Teleport`, `Step`, `IsGrounded`, `GetPosition`, `GetVelocity` and a getter and setter for each setting; every argument is checked (a Lua error for a wrong type, a non-finite number or a value out of range). `engine-api.lua` documents it.
- `BodyState` (`GetBodyState`, `SetBodyState`) and the capsule cast and overlap were already on both backends (PR #199, #205, #212); new typed suite `BodyStateTests.cpp` runs the round trip on PhysX and Jolt, with the exact-roundtrip drift check, kinematic and static bodies, sleep and wake, refused states, and the event side effects of a teleport (one Enter in, one Exit out, nothing for writing back the same state).
- Tests: `CharacterControllerTests.cpp` (typed: flat ground, walking, falling, jump, ceiling, walls and corners, steps up and down, ledges, walkable and steep slopes, layers, triggers, determinism over a scripted 400-tick run twice, save and restore, corner cases), `LuaCharacterControllerTests.cpp` (typed).
- Docs: `docs/physics.html` "Character controller" with the design-decision callout and the Not verified list, `docs/scripting-lua.html`, `docs/testing.html`, `docs/index.html`.

## Decisions for you to accept
See the callout `design-character-controller` in `docs/physics.html` (also in the index): the controller is a query shape with no body of its own; a step carries the capsule at least one radius forward; the hash is the check for determinism and the two backends are not compared bit for bit.

## Not verified
- Nothing was built or run locally; CI (12 checks) is the build and is green at 57f7a7ca (the typed controller, BodyState and Lua tests ran on PhysX and Jolt).
- Not tried: the controller against the Jolt thread pool, moving platforms, pushing dynamic bodies, crouching, a capsule that turns, obstacles thinner than one move.
- PhysX and Jolt are not compared bit for bit (their sweeps differ by millimetres).

Review round: a step edge is now a wall to the horizontal slide (stepOffset is the climb limit), sub-millimetre moves can step, docs/testing.html rows and the recovery reach text fixed. Not built locally; CI pending.

Second review round (c505f881): a ramp crest ending in a flat top is no longer treated as a step edge (a behind-contact raycast finds the walkable ramp), and a step-up landing balanced on the edge of something taller than stepOffset is refused. New test WalksOverTheCrestOfARampOntoAFlatTop (stepOffset 0 and 0.3). Not built locally; CI pending.

## Approved deviations
Accepted by the user 2026-10-13:
- GetBodyState/SetBodyState and capsule cast/overlap already existed on both backends (PR #199/#205/#212); this PR adds the typed BodyState suite, no backend change
- Controller is a query shape with no body of its own and does not exclude its own colliders (layer mask does)
- move is held, jump is one press; position is the capsule centre; height includes the caps
- Determinism proven per backend (same inputs, same hash; restored state replays); PhysX vs Jolt not compared bit for bit
- Round 3 redesign: horizontal slide climbs a walkable contact only if a ray just behind it finds ground (rim of a step = wall); step-up accepts ground incl. the rim of walkable ground (ray just beyond, past Jolt edge rounding 0.05) and refuses support higher than stepOffset above the start bottom; ground snap stands on rims (a character stopping on an edge stays balanced on it); no forward carry
- Edges lower than skin*(tan(slopeLimit)+0.5) (3 cm default) are ridden over like a slope even with stepOffset 0
