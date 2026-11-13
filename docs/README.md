# N2Engine documentation

This folder is a static HTML site that documents how the engine **behaves**: guarantees, ordering, lifetimes, edge cases and error handling. Start at [`index.html`](index.html).

- No build step, no external fonts or scripts. Everything is plain HTML plus `assets/style.css` and two small scripts: `assets/highlight.js` colours the code blocks (`<pre class="lang-*">`) and `assets/search.js` adds the search box (see [Search](#search)). The pages read fine without either.
- Works opened straight from disk (`file://`) and on GitHub Pages (serve the `docs/` folder from the default branch). All links are relative.
- Light and dark themes follow the reader's system setting (`prefers-color-scheme`).

## Search

Every page has a search box in the top right (on narrow screens, a search button in the top right corner that opens a bar across the top). `/` or Ctrl+K (Cmd+K on macOS) focuses it, the arrow keys move through the results, Enter opens one and Escape closes the list. Each result shows the page title, the nearest `h2`/`h3` with an `id`, and a snippet with the match highlighted; it links to `page.html#that-id`. A section matches when it contains every word typed (anywhere in its page title, heading or text); headings weigh more than body text.

There is no server. `assets/search.js` loads a prebuilt index, `assets/search-index.js`, the first time the box is used. The index is a script (it sets `window.N2DocsSearchIndex`) rather than JSON so the search also works from `file://`, where browsers block `fetch`.

**Regenerate the index after changing any page** (plain Node 16 or later, no npm packages):

```sh
node docs/tools/build-search-index.mjs           # writes docs/assets/search-index.js
node docs/tools/build-search-index.mjs --check   # exits 1 if the committed index is stale
```

The generator reads every `docs/*.html` except `_template.html`, takes the text inside `<main>` (without the "On this page" box and the page footer) and makes one record per `h2`/`h3` that has an `id`; text before the first one belongs to the page itself. A heading without an `id` stays in the section before it, so give headings ids if they should be search targets. Line endings are normalised, so Windows and Linux checkouts produce the same file.

The stale-index check runs on every docs deployment: `vercel.json` sets the Vercel build command to `node tools/build-search-index.mjs --check`, so a pull request whose index is out of date fails its `docs (Vercel)` check and gets no preview, and master cannot deploy a stale index.

## Current baseline

The pages describe `master`. PRs #26–#34 have all merged; their `PR #nn` badges stay as history, pointing readers at the PR that introduced or changed a behaviour. Every page's footer says "Describes master through PR #34."; update it on every page (and in `_template.html`) when a later PR brings the pages up to date.

## Pages

| File | Covers |
|---|---|
| `index.html` | Overview, module list, architecture map, ownership, frame summary, cross-cutting rules |
| `application.html` | Init/Run/Shutdown, frame order, fixed step, Quit, health, headless mode, renderer resources, the software renderer's headless mode |
| `gameobjects-components.html` | GameObject/Component lifecycle, hierarchy, destroy, component lookup and removal |
| `transforms.html` | `Positionable` local/world transforms, dirty propagation, physics notification |
| `scenes.html` | `Scene` and `SceneManager`: storage, loading, switching, teardown |
| `coroutines.html` | Coroutine scheduling, waits, stop rules, re-entrancy |
| `physics.html` | Backends, bodies, colliders, events, raycasts, mass properties, the vendored Jolt Physics (option, build flags) and the Jolt backend (core: what exists, the mapping, the differences from PhysX, determinism declarations) |
| `input.html` | Input system, action maps, bindings, value combination |
| `audio.html` | Audio system, sources, one-shots, mixer groups, shutdown |
| `text.html` | Font backends, SDF atlas, UTF-8 and text layout, the `Font` asset and default font, the `TextRenderer` component, the OpenGL and software text shaders, golden-image tests, the drawing code shared with UI text |
| `ui.html` | Screen-space UI: canvases, `RectTransform` layout, `Image` (and its sprite), `UIText`, the UI pass, UI picking |
| `particles.html` | The `ParticleSystem` component, the `IParticleBackend` seam and backend selection (`Auto`/`PhysX`/`CPU`), the PhysX GPU (PBD) backend, its driver seam, presets and limits, generation-counted handles, the CPU simulation, play-mode rules, billboard drawing, the Particles health row, Lua, running the GPU test |
| `soft-bodies.html` | The `SoftBody` component, the voxel tetrahedraliser and its cache, the CPU XPBD backend and its limits, backend selection (`Auto`/`PhysX`/`CPU`), the PhysX deformable-volume backend (cooking, driver seam, differences, limits, failure, running the GPU test), the runtime mesh override, the SoftBodies health usage, Lua |
| `gpu-softbodies-checklist.html` | What to run by hand on a Windows GPU PC to check the PhysX soft-body backend (build, the GPU test, a scene, material model and fallback checks, expected output) |
| `gpu-vulkan-checklist.html` | What to run by hand on a Windows GPU PC to check the Vulkan renderer's window path (build, the Vulkan tests on the GPU, `lua_project --renderer vulkan` per station against OpenGL, resize/minimise/Alt-Tab/multi-monitor/HiDPI, vsync, two GPUs, a validated run, performance, what to report) |
| `gpu-particles-checklist.html` | What to run by hand on a Windows GPU PC to check the PhysX particle backend (build, the GPU test, a scene, expected output) |
| `meshes.html` | Texture assets and their import settings, the `assetimport` library, the UV convention, `GpuCache`; meshes, materials and models in later phases of #3 |
| `resources.html` | Resource paths, `ResourceLoader`, `Resources` |
| `serialization.html` | Scene/component JSON, references, failure handling |
| `math.html` | Math types, SIMD tiers, correctness guarantees |
| `scripting-lua.html` | `LuaComponent`, fields, checked handles, subscriptions, `engine-api.lua` |
| `editor-extensions.html` | Lua editor extensions: the model (Lua in the edit host, the UI as data, no code in the editor), `--editor-extensions`, the trust prompt and `--editor-extensions-watch`, the file layout, what a file declares, forms and field kinds, menu items, inspector sections, hooks, the whole `Editor` API, capabilities and the role table, budgets, workers and the fault flag, the sandbox, security notes, troubleshooting (states and messages), the AlignTools example, `editor-api.lua` |
| `logging-and-editor.html` | Logger, engine health, the editor host |
| `editor.html` | Editor projects: `project.n2proj`, `--create`, opening a project, scene files, unsaved changes, the hierarchy and entity commands, project settings, per-project `user://` |
| `mcp.html` | The `n2-mcp` bridge (a separate program in the frontend repository): running it (`--launch`, `--connect`, flags), the safety model, the tools that exist, sharing the host with a person and the Hello `yield` flag, what is not built yet |
| `netcode.html` | n2net, the netcode packet layer: the `ITransport` interface, the packet byte layout, acks, the reliable-ordered and unreliable-sequenced channels, fragmentation and its limits, bounded parsing of untrusted input, the deterministic network simulator and how to use it in tests, the native UDP transport, the secure layer and N2Server, the network world (the `NetworkObject` component, net ids, scene objects matched by UUID, prefab spawns and despawns, the join fingerprint, the world messages, `NetworkServer` and `NetworkClient`), field replication (`.Replicated()`, Lua `replicated = true`, the codecs, deltas against the acknowledged baseline, `NetworkTransform`), what is not built yet (interpolation, inputs, RPCs, prediction) |
| `web-build.html` | The WebAssembly build (stages 0 to 2 of #155): Emscripten toolchain and pinned version, `N2ENGINE_PLATFORM_WEB`, the dev/size/speed profiles, building locally, the reduced target set and exclusions, the web player (canvas, loop, input, assets, page), building and hosting the static package, the output contract, the size report, what is not done yet |
| `web-checklist.html` | What to check by hand in a real browser for the web player (loading, canvas, input, resize, hosting from a subdirectory, MIME fallback, performance), which CI cannot do |
| `build-targets.html` | Build targets: `n2build` (list, doctor, build, run, `--json`), the windows, linux and web targets, build trees and packages, the desktop player (`players/desktop`), the physics check per target, target descriptors |
| `testing.html` | Test layout, CI, conventions, pixel tests with the headless software renderer, the Jolt smoke test and its cross-leg state hash, the typed physics tests over both backends and the Jolt backend determinism numbers |

## Adding or changing a page

1. Copy `_template.html` to `<topic>.html`. It contains the full page skeleton: head, sidebar, "On this page" box, callouts, footer. Delete its "Authoring template" callout and comment in the copy. The template itself is not linked from the site (and Jekyll is disabled with `.nojekyll`, so GitHub Pages serves it as-is).
2. Set `<title>` to `<Page title> · N2Engine docs` and add `aria-current="page"` to the page's own sidebar link.
3. Add the new page to the sidebar of **every** page. There is no JavaScript to generate it, so the sidebar is repeated verbatim in each file. Keep the order and grouping identical everywhere.
4. Add the page to the table above and, if it is a subsystem, a card on `index.html`.
5. Keep the `<script src="assets/search.js" defer></script>` line from the template, and regenerate the search index (see [Search](#search)).

## Writing conventions

- **Behaviour first.** For each topic, state what is guaranteed, in which order things happen, how long objects live, what is an error and how it is reported (log, return value, exception, Lua error), and why. Leave full signatures to the headers.
- **Cite sources** after a section or claim group, without line numbers:
  `<p class="src">Source: <code>engine/src/Positionable.cpp</code> — <code>Positionable::SetScale</code>; tests: <code>ZeroParentScaleKeepsLocalValue</code></p>`
- **Callouts** (`<div class="callout KIND"><p class="callout-title">Title</p>…</div>`):
  - `guarantee`: enforced by the code, ideally pinned by a named test.
  - `note`: rationale, context, or a consequence worth knowing.
  - `warning`: intended but surprising behaviour, or an unsupported pattern.
  - `open-question`: unclear, undecided or inconsistent behaviour. Name the file and function. Never guess; an open question is better than invented behaviour.
  - **Design decisions** are `note` callouts titled `Design decision: …` with an `id="design-…"`. Explain *why* a behaviour was chosen, once, on the page that owns it, and link to it from elsewhere. `index.html#design-decisions` lists them all; add new ones there.
- **Badges** mark the PR that introduced or changed a behaviour: `<span class="badge">PR #28</span>`. Use them on subsystem pages; on `index.html` only the one example in "What these docs cover" that explains them.
- **Code samples** use `<pre class="lang-cpp"><code>…</code></pre>` (also `lang-lua`, `lang-json`, `lang-text`, `lang-cmake`, `lang-shell`). Escape `<`, `>` and `&`. Keep samples short and correct; prefer patterns from the tests or example projects.
- **Tables** go inside `<div class="table-wrap">` so they scroll on narrow screens.
- **Ordered sequences** (frame order, teardown order) use `<ol class="steps">`.
- **Links** between pages are relative (`physics.html#events`). Give every `h2`/`h3` you might link to an `id`.
- **Style:** short sentences, reference tone, no marketing. Files use LF line endings and UTF-8.

## Keeping the docs true

- A PR that changes engine behaviour should update the affected page in the same PR.
- Code wins over prose. If a page and the code disagree, fix the page, or open an issue if the code looks wrong.
- When an open question is resolved, replace the callout with the decided behaviour, and cite the test that pins it.
