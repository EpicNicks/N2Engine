Accepted by the user 2026-10-13:
- Dawn consumed as the pinned prebuilt release archive (v20261016.190333, SHA-256 checked) instead of a source build: reproducible, no depot_tools deps, no Dawn compile in CI
- Option is a configure error off Linux x86_64 (Dawn publishes one archive) and forced OFF for web
- RenderBackend::WEBGPU not added: tests construct WebGpuRenderer directly, so W8 holds by construction
- Test fixture also fails tests that leave a WebGPU reference behind (live-object count), beyond the plan
- CI job webgpu (Dawn, lavapipe) measured 1m13s on first run (no Dawn compile)
- Added SetMaxBufferSizeForTest to WebGpuRenderer so a test can reach the readback buffer-limit check
- CI cost stated in docs as about one minute (1m13s, 1m03s measured)
