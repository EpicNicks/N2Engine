# Notes (particles-component-cpu, 2026-10-13)
Approved deviations (accepted by the user):
- IParticleBackend gained GetName, Clear, Step, GetAliveCount beyond the five calls in the plan (CPU stepping, burst and tests need them); Step is a no-op for a backend that simulates inside the physics scene.
- CopyPositions returns Vec4 with w = age fraction (not invMass), so size and colour over life need no per-particle attributes; per-particle random size/colour ranges are therefore start/end values over life, with random ranges only for lifetime and speed.
- Each ParticleSystem owns its own CpuParticleBackend (stepped in OnFixedUpdate) instead of a shared one; the GPU backend is re-fetched from the physics backend each use so it never dangles.
- Emitter is world-space only (no local simulation space), cone axis is the object's local +Y, no looping/duration fields.
- CPU collision is the optional ground plane only (no raycasts), as the plan allowed; documented.
- Added RenderState::additive (OpenGL SRC_ALPHA,ONE; software ignores) and Rendering::RenderCamera (Scene::Render sets the camera current) because IRenderable draws get no camera.
- Application::GetHealth is now non-inline and appends 'ParticleSystems: N CPU' to the Particles row detail only when systems exist (row state unchanged, no PR-A test affected).
- ParticleSystem reports no world bounds (editor picks by sphere); no editor-mode preview.
- Edited EditorInspectorTests and ReflectionTests builtin lists to include ParticleSystem; PR-number badges in docs say PR #125.
- Review fix: a PhysX-backed system whose physics backend is replaced (another instance) drops its handle and restarts empty on CPU with one warning; removal of the physics backend entirely still leaves it inert as documented; identity is by particle backend pointer.
Known limit: stale-handle guard compares backends by address; PR-C (PhysX PBD backend) must use a generation counter. PR-C needs the user's Windows GPU PC to verify; Vulkan renderer is stubs (draws nothing, warns once). Unverified: OpenGL rendering of particles and additive blend on a real GPU; Vulkan renderer (stubs; draws nothing, warns once); PhysX GPU backend (next PR); GetParticleBackend only exercised through a fake
