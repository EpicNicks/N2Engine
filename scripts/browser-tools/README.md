# Browser gate tooling (G6)

Development/CI only; none of these packages or Chromium ships in the player. `package-lock.json` pins Playwright 1.64.1, its Chromium 156.0.8078.4 revision, and PNG decoding. Update the exact version and lock together, install the matching browser, and run the gate before committing a bump. CI uses Node 22 and Ubuntu 24.04.

From the repository root:

```sh
npm ci --prefix scripts/browser-tools --ignore-scripts
node scripts/browser-tools/node_modules/playwright/cli.js install --with-deps --no-shell chromium
export N2_READBACK_DIR="$PWD/build-web/browser-tests"
export N2_BROWSER_EVIDENCE_DIR="$PWD/build/browser-evidence"
xvfb-run -a node scripts/web-browser-smoke.mjs build-web-size/web-package scripts/browser-tools --swiftshader --headed
```

Build `web-package` in the size profile and `N2WebGpuReadbackTests` in a test-enabled web build first. Linux needs Xvfb and xauth (`apt-get install xvfb xauth`). The CI browser job downloads these exact outputs from its own workflow run instead of rebuilding the engine. Missing inputs fail; a SwiftShader run cannot skip the deterministic readback golden.

The full Chromium channel is used instead of the legacy headless shell. Linux CI runs it in a virtual display: raw headless WebGPU canvas compositing produced black frames or device loss locally even for a minimal red-clear probe. Xvfb with the Vulkan compositor rendered correctly. This still needs no physical GPU: `--use-webgpu-adapter=swiftshader` forces Dawn's CPU fallback, and `--use-vulkan=swiftshader` selects the software Vulkan implementation for the compositor. The runner checks reported adapter identity so hardware cannot silently substitute. These switches are documented in [Chromium's GPU switches](https://chromium.googlesource.com/chromium/src/+/main/gpu/config/gpu_switches.cc), [GPU service switches](https://chromium.googlesource.com/chromium/src/+/main/gpu/command_buffer/service/gpu_switches.cc), and [headless GPU guidance](https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/using-gpu-hardware-in-headless-chrome.md).

Default invocation remains headless with normal browser selection. `--unsafe-webgpu` only enables otherwise-blocked adapters; it does not assert SwiftShader. `N2_BROWSER_EXECUTABLE` supports manual runs with installed Chrome/Edge; CI leaves it unset to use the locked Chromium.

The animated demo is checked for nonuniform composited canvas output, not a timing-dependent screenshot hash. Exact pixel checks use `tests/browser/WebGpuReadback.cpp`'s fixed-colour scene: clear/alpha, row padding, orientation, resampling, resize and pending-map destruction. Negative controls cover missing inputs, hardware substitution, blank images, and noisy corners.

Evidence contains browser/tool versions, adapter, launch arguments, passed stages, screenshot pixel counts, page errors, readback result and console/request logs. Failed runs retain `failure.png` and `failure-trace.zip` when a page exists; input/launch failures still write `report.json` and `browser.log`. CI uploads evidence for 14 days. Inspect a trace with the same pinned tool's `show-trace` command. Real GPU drivers, mobile, Safari/Firefox and physical device resets remain separate checks.
