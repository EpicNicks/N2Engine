Soft bodies PR-C: the PhysX GPU deformable-volume backend behind `ISoftBodyBackend`, with all the logic tested against a fake driver on every CI leg. Hold for the user's GPU run (hardware rule, as particles PR-C #129).

## What it adds
- `SoftBodies::IDeformableDriver` (no PhysX/CUDA types, main thread, outside a scene step), mirroring `IPbdDriver`: `CreateVolume`, `DestroyVolume`, `GetStaging`, `GetNodeMap`, `SetMaterial`, `Upload`, `ReadBack`.
- `SoftBodies::DeformableSoftBodyBackend` over it (name `PhysX`): generation-counted handles, volume cap and refusal -> failure state, `SetParams` -> material, `AddVelocity` as a velocity read-modify-write, one read-back per body per scene step (`OnSceneStepped`), `HasFailed`/`GetFailureReason`/`ReleaseAll`. `Step` is a no-op: the scene steps the volumes.
- The render vertices are skinned from the volume's nodes by `SoftBodies::SkinSurface`, the same code the CPU backend now calls (`TetSkinning.hpp`), so the existing `surfaceEmbedding` stays valid.
- `Physics::CookDeformableMesh` (own TU, compiled whenever PhysX is linked, no GPU needed): cooks the engine's own tetrahedra (as collision and simulation mesh, `buildGPUData = true`) from the WORLD-space rest shape, so a mirrored or non-uniform transform is handled by `OrientTets` (swap two corners) instead of `PxDeformableVolumeExt::transform`; builds a node map if cooking reorders vertices.
- `Physics::PhysXDeformableDriver`: the only file with PxDeformableVolume / CUDA calls, whole file under `N2ENGINE_PHYSX_GPU_PROBE` like `PhysXPbdDriver`.
- `PhysXBackend`: `_softBodies` beside `_particles` (same GPU-scene condition), `GetSoftBodyBackend()` polls the failure, `GetSoftBodiesGpuStatus()` passes `hasBackend = _softBodies != nullptr`, a driver failure calls `FailSoftBodies` only (scene status, Particles row untouched), released before the particles, the scene and the CUDA context, `InstallDeformableDriverForTesting` seam.
- `SoftBodyParams` gets `materialModel` and `selfCollision` (`SoftMaterialModel` moved to its own header); the CPU backend ignores them.
- Tests: fake-driver backend logic, cooking on every PhysX ON leg, PhysXBackend wiring + status split, SoftBody component and Lua over the PhysX backend with a fake driver, and the GPU golden test `PhysXGpuSoftBodiesGoldenTest.*` (registered, skipped in CI).
- Docs: `docs/soft-bodies.html`, `docs/physics.html`, new `docs/gpu-softbodies-checklist.html`.

## Not changed
No protocol change, no pinning, no Linux GPU asset or PhysX package workflow, no root CMake PhysX logic. `physics.gpu=off` keeps today's CPU scene (regression test).

## Not verified
Everything that touches a real CUDA context: creating a volume from the cooked mesh, whether `fetchResults(true)` alone makes the device buffers safe to read, read-back timing, VRAM and performance with N bodies, CoRotational/NeoHookean stability on real meshes, PGS solver quality, one GPU module for particles and deformables, DLL-missing behaviour, and that cooking the same tetrahedra as collision and simulation mesh is accepted by a GPU scene (cooking itself is tested in CI). See `docs/gpu-softbodies-checklist.html`.
