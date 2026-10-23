Accepted by the user 2026-10-13:
- GetBodyState/SetBodyState and capsule cast/overlap already existed on both backends (PR #199/#205/#212); this PR adds the typed BodyState suite, no backend change
- Controller is a query shape with no body of its own and does not exclude its own colliders (layer mask does)
- move is held, jump is one press; position is the capsule centre; height includes the caps
- Determinism proven per backend (same inputs, same hash; restored state replays); PhysX vs Jolt not compared bit for bit
- Round 3 redesign: horizontal slide climbs a walkable contact only if a ray just behind it finds ground (rim of a step = wall); step-up accepts ground incl. the rim of walkable ground (ray just beyond, past Jolt edge rounding 0.05) and refuses support higher than stepOffset above the start bottom; ground snap stands on rims (a character stopping on an edge stays balanced on it); no forward carry
- Edges lower than skin*(tan(slopeLimit)+0.5) (3 cm default) are ridden over like a slope even with stepOffset 0
