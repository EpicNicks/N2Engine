# Things only you can verify (as of 2026-10-15)

CI proves compilation, unit tests, the host smoke tests (Windows, Linux x86_64 and arm64, Docker) and PhysX linking. It cannot prove the items below. Tick them off as you get to them; tell the orchestrator and it will log them in the progress log.

## Needs a GPU (Windows PC)
- [ ] OpenGL text rendering with multi-pass effects: the pass order sort and the multi-draw path (engine #110, #111).
- [ ] Any OpenGL rendering path after the Linux/PhysX changes (CI never runs the GPU code, it only compiles it).
- [ ] After the particle GPU probe (PR #122): with `physics.gpu = auto` on the Windows GPU PC the probe creates a CUDA context and the GPU scene; with `off` nothing changes. Health row "Particles" shows Ready/Disabled correctly.
- [ ] **Particles PR-C (engine PR #129, HELD until you report the result).** Build with PhysX ON on the Windows GPU PC and follow docs/gpu-particles-checklist.html in the PR branch (feat-particles-physx-pbd): run the GPU golden test (it skips itself unless a CUDA GPU and the GPU module are present), run the sample scene with ParticleSystem backend PhysX, and confirm: particles spawn, fall under gravity, respect the cap, the Particles health row says "PhysX GPU: <device>", exit/destroy is clean. Also check what the review could not: PBD preset values (water/sand/smoke) look right, VRAM use, and the cost of the per-step read-back. With physics.gpu=off nothing may change (rigid bodies behave as before). Tell the orchestrator the result; it then updates the branch, re-runs CI and merges.
- [ ] **Soft bodies PR-C (engine PR #146, HELD until you report).** Build branch feat-softbody-physx with PhysX ON on the Windows GPU PC, run PhysXGpuSoftBodiesGoldenTest.* (it skips itself without a CUDA GPU) and follow docs/gpu-softbodies-checklist.html. With the full lua_project scene the health row should read 'SoftBodies: 8 PhysX' (6 demo-station bodies + the checklist snippet's 2 jellies), or 6 without the snippet; mixed backends read '1 CPU, 7 PhysX' style. Especially check: AddVelocity visibly pushes a body that has been resting about 4 s (the wake-counter fix), the robot Body tetrahedralises (else its sign says 'inert'), and the section 4 manual step (soft bodies keep moving after the GPU particles are switched off). Report the result; then I record your acceptance of its deviations (no PhysX-pipeline fallback, density read only at creation, 4095-volume refusal fails the whole feature) and merge.
- [ ] **Demo stations (engine #137, #145)**: look at them on your PC: particles station (keys 8/9/0 first list: B burst, P play/stop, C clear, K cycle backend), the soft-body station (key '-': R drop again, J cycle backend, V push up), the world-canvas trigger-vs-solid station and the text-effects pass-order station. Check layout, sizes, solver stability at 1e6 Pa / resolution 8, whether signs read 'CPU' / 'PhysX GPU' correctly (PhysX only with physics.gpu auto/on and a CUDA GPU).
- [ ] **Vulkan (issue #43)**: stages 0, A, B are merged and verified on lavapipe (software Vulkan) in CI only; stage C (materials/lighting), D (blending/queues), E (text) are in progress; stage F (window, swapchain, present) is the first that needs your GPU: it will come with docs/gpu-vulkan-checklist.html and be HELD for your run. Until F, Vulkan cannot show a window; OpenGL stays the default.

## Needs a real Linux machine
- [ ] Build and run N2EditorHost on a real Linux box (not CI): ready line, TLS identity folder permissions (dir 0700, key 0600), project in `$XDG_DATA_HOME/n2engine` or `~/.local/share/n2engine`.
- [ ] The systemd unit (deploy/n2editorhost.service) on a real machine (DynamicUser, StateDirectory, token via EnvironmentFile, libopenal path).
- [ ] The tokens-file unit (deploy/n2editorhost-tokens.service, engine #133): one-time `useradd --system --no-create-home --shell /usr/sbin/nologin n2editor`, tokens file owned by n2editor with mode 0600 (the host refuses any other mode), `--new-token alice editor` output added to the file, `systemctl reload` (SIGHUP) picks up a new or revoked token without a restart, ReadWritePaths/StateDirectory are enough for the project.
- [ ] Docker/Compose/Swarm secrets for the tokens file: the secret really arrives with uid/gid/mode 0400 (the default 0444 is refused), `docker kill -s HUP <container>` reloads, `docker run --rm <image> --new-token alice editor` works. CI only tests revocation by file rewrite in the docker leg, never SIGHUP (the host is PID 1 there).
- [ ] Exposed-host hardening on a host reachable from beyond loopback (engine #126, #130, #132): roles (viewer cannot write or play, editor cannot Shutdown), failed-Hello backoff from a real remote address (3rd failure starts 1 s doubling to 5 min), loopback/tunnel global burst rule (10 failures/min) with a real ssh tunnel, audit lines carry user/role/peer. Release-host log visibility: see the durable-logs item below once merged (journalctl -u n2editorhost / docker logs).
- [ ] The arm64 Docker image build (CI builds the x86_64 image only).
- [ ] PhysX on Linux under real gameplay (x86_64), and the aarch64 build under load (CI runs the tests only). Debug builds on Linux link the release PhysX libraries (diagnostics only).

## Needs a real remote host
- [ ] Remote editing over a real ssh server: the frontend waits for ssh's `Allocated port N for local forward` line, never observed on a real server.
- [ ] TLS direct-address connection to a real cloud host: first-connect fingerprint confirmation, Forget pin, changed-fingerprint refusal, WAN throughput.
- [ ] Windows ACLs on the TLS key file and on a tokens file (never checked on Windows; the 0600 permission check is POSIX only).
- [ ] NEON on real arm64 hardware beyond CI (engine #128): performance vs scalar (no benchmark exists) and the effect of `-ffp-contract=off` (applied PUBLIC on aarch64, so everything linking math loses fused multiply-add).

## Needs the Electron editor run against a live host
- [ ] The editor UI built in F3-F7 and the OpenGL play child have never been run against a live host. (Tracked in GitHub issue #149 'Editor: first live-host verification'; issue #6 is closed.)
- [ ] The direct-remote form (TLS checkbox, Inspect, Forget pin) and the inspector pass-list editor were typechecked and bundled but never run in Electron.
- [ ] Read-only and roles in the real Electron app (frontend #28, #32): against a host started with `--read-only` and against a tokens-file host: the read-only badge and user/role badge show, all write controls and Play are disabled, a refused command shows the host's message in the banner. In particular confirm the error code really survives Electron's contextBridge (the code travels as a U+2063-delimited message prefix; it was only simulated in tests) and that the optional User field in the connect form works (SSH recent remotes remember the user, the token is never stored).

## Needs a file from you
- [ ] A Blender-exported binary FBX for `fbx-blender-fixture` (see tests/assets/models/README.md).

## Needs a setting from you (GitHub)
- [ ] (Optional, my CI notes) 'Require branches to be up to date' is NOT enabled today: the extra full CI run after every 'update branch' comes from the orchestrator's own merge gate, not from GitHub. Tell me if you want that gate relaxed (I will not change it myself).
- [ ] Branch protection on master: required status check named exactly `linux (Release, x86_64, Clang)`. Not required: the arm64, GCC 14 and docker jobs.
