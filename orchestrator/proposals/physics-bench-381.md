# Jolt and PhysX measurements, and what #381 items 7 and 10 settle

**Outcome (2026-11-23):** item 7 done in #435 (this bench runs in CI and prints Jolt-vs-PhysX and the Jolt snapshot cost as job-summary tables; `PhysicsCorpusThreadsTest` on 10 corpus scenes; the corpus grew to 14 scenes); item 10 done in #385 and #436 (elementwise ops bit-exact in every SIMD tier, the rest recorded in `docs/math.html#simd-why-tolerance`). #381 is closed. The numbers below are this one-machine measurement; the CI tables are the current ones.

Measured on one Windows 10 machine, MSVC Release, `N2ENGINE_STRICT_FP` at its default for Windows (off), both backends
with `SetDeterministic(true)` (single-threaded). One run, so treat the numbers as an order of magnitude, not a benchmark
suite. Harness: `tests/engine/physics/PhysicsBackendBenchTests.cpp` (prints `[BENCH]` lines, asserts only that every
operation works). Scene: a floor and N mixed boxes, spheres and capsules dropped in layers of 25; 120 warm-up ticks, then
300 measured ticks of 1/60 s (the full `ApplyPendingChanges`, `Update`, `SyncTransforms`, `ProcessCollisionCallbacks`
sequence).

## Step time (microseconds per tick)

| bodies | Jolt median | Jolt worst | PhysX median | PhysX worst |
|-------:|------------:|-----------:|-------------:|------------:|
| 25     | 1.1         | 1.3        | 6.7          | 10.8        |
| 100    | 207.3       | 315.7      | 218.4        | 336.8       |
| 400    | 1096.9      | 1386.1     | 884.1        | 1074.8      |

Jolt and PhysX are within about 25% of each other at 100 and 400 bodies, PhysX ahead at 400. The 25-body row is a
settled, sleeping pile (one layer lands and sleeps inside the warm-up), so it measures the cost of an idle world, not
simulation. A scene that stays active, a different body mix or a multi-threaded step would give other ratios; none of
those were measured.

## Jolt snapshot cost (median microseconds, 200 repeats, after the 300 ticks)

| bodies | snapshot bytes | SaveState | RestoreState | HashState |
|-------:|---------------:|----------:|-------------:|----------:|
| 25     | 6445           | 14.1      | 29.7         | 2.7       |
| 100    | 45701          | 94.1      | 179.7        | 10.5      |
| 400    | 182701         | 394.4     | 889.9        | 43.0      |

A save is about 0.1 of a step at 100 and 400 bodies; a restore is about 0.8 of a step. A rollback of N ticks costs the
restore plus N steps, so the restore is not what limits the rollback window. Snapshots are of a settled pile, which is
the cheapest case for the byte count; PhysX has no snapshot to compare with.

## Item 7: thread-count independence

Already proven by `JoltSnapshotTest.TheStateDoesNotDependOnTheJobSystemsThreadCount` (and `ARollbackUnderWorkerThreads...`),
so the issue's "unproven" is stale. Re-run here: 0 workers (single-threaded job system) against 1, 2 and 4 workers gives
the same hash after 150 ticks (`0x1c5b4d2f43ac0f66`), with the contact listener seen on 2, 3 and 5 threads. Scope is one
scene on one binary; the preset still stays single-threaded.

Not done: growing the 10-scene determinism corpus. Every added scene needs its hashes in `tests/golden/
physics-determinism.json`, which `scripts/physics-determinism.py golden` regenerates only from the Linux x86_64 Clang
leg, so a scene added from this machine would fail the CI comparison until that regeneration is committed.

## Item 10: SIMD tiers

`SimdDispatchTests` compares every tier with scalar within 1e-5 because a SIMD path may sum in another order or divide by
a reciprocal. Added `ElementwiseArithmeticIsBitIdenticalToScalar`: Vector3 negate, scalar multiply, add and subtract are
single IEEE operations and must be bit-identical in every tier (passes on SSE2, SSE41, AVX here; NEON was not run). Dot,
Length, Normalized, Cross and scalar divide stay tolerance-only on purpose (order or fusion differences), which is why the
Deterministic preset requires `math.simdTier` scalar; making them bit-exact would mean giving up the SIMD speed those
tiers exist for.
