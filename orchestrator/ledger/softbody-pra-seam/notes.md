Accepted by the user 2026-10-13:
- Physics.IsGpuAvailable no longer turns false when only the particle backend fails (scene status is no longer rewritten); intended by the plan, but it is a behaviour change from #129
- Existing particle failure-chain tests keep their assertions but read GetParticlesGpuStatus instead of GetGpuStatus after a particle failure, and add scene-stays-Ready and SoftBodies-unchanged checks
- Added IPhysicsBackend::GetParticlesGpuStatus/GetSoftBodiesGpuStatus and PhysXBackend::FailSoftBodies (test hook, PR-C will call it); ParticleBackendSelection fallback text now reads GetParticlesGpuStatus (same text as before)
- On a Ready scene with no soft-body backend the SoftBodies row is Disabled 'GPU soft bodies unavailable: no PhysX soft-body backend in this build' (plan wording covers only off/no GPU); wording is my choice
- Seam is minimal: SoftBodyDesc holds only params (tet mesh/embedding joins in PR-B); no AddVelocity/SetPinnedTargets; 'status' is not a seam method, health goes through IPhysicsBackend::GetSoftBodiesGpuStatus
- Handle helper is BackendHandle.hpp with a per-feature generation counter (template tag) so particle generations are unchanged; Particle names kept as thin wrappers
- Health order is now Window, Renderer, Physics, Particles, SoftBodies, Audio, Scripting; docs/application.html updated
- Review fixes: smoke check asserts the SoftBodies row is Disabled; InstallParticleDriverForTesting also clears the particle failure (restores the Particles row after re-install, as before #138) with a test; docs wording says the PARTICLE GPU status changes on a refused transfer. No Application-level health test exists to extend (only GpuProbeTests mapping tests).
