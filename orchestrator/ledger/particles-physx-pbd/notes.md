Accepted by the user 2026-10-13; merge HELD for the user GPU run.
- ParticleSystemHandle widened from uint32 to uint64 (generation<<32 | index); IParticleBackend base class issues the generation and OwnsHandle; CpuParticleBackend and the test fake updated to match
- Explicit backend PhysX with no GPU falls back to CPU with one warning (as PR-B defines and the plan says 'exactly as PR-B defines'), not inert as the brief's sentence reads; a GPU backend that refuses CreateSystem also falls back to CPU in ParticleSystem::OnAttach
- Added ParticleSystem.pbdMaterial (Water/Sand/Smoke) component field and Lua SetPbdMaterial/GetPbdMaterial to select the per-system preset; no protocol change (ordinary enum field)
- PBD ageing and compaction run on the host after every scene step (PhysXBackend::Update calls PbdParticleBackend::OnSceneStepped); Step() is a no-op; host mirror is refreshed by read-back each step so spawns never upload stale positions
- Real driver compiled only where N2ENGINE_PHYSX_GPU_PROBE (Windows with GPU library), not on Linux ON legs whose static libs are CPU-only; new header PhysXGpuSupport.hpp holds the macro that PhysXBackend.cpp defined locally
- GPU golden test also skips when GITHUB_ACTIONS is set (so CI never loads the GPU library), in addition to the ordinal and Ready checks
- Application::GetHealth now re-reads the physics GpuStatus for the Particles row state and base detail so a later GPU failure shows; Ready detail changed to 'PhysX GPU: <device> (CUDA device N)'
- PBD material lift and drag are not changeable at run time (only deprecated setters); ParticleParams.drag is added to the preset's damping instead; ground plane and bounce are ignored by the PhysX backend
- Manual checklist is docs/gpu-particles-checklist.html and edits lua_project temporarily rather than adding a separate sample project
- Did not add a byte-for-byte rigid-body regression test for physics.gpu=off (scene creation code path for gpu off is unchanged; tests assert no particle backend and Disabled status)
- PR body updated through the REST API because gh pr edit needs read:org scope the token lacks
- Review fixes: fetchResultsParticleSystem() after fetchResults when PBD exists; null-guarded CUDA device name in ProbeGpu

Hold released 2026-10-14: the user reported the GPU test + checklist worked on the Windows GPU PC. Branch updated to master (head 0513750); needs CI + re-review of the new head, then ratify --merge.
