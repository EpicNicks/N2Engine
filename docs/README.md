# N2Engine documentation

This folder is a static HTML site that documents how the engine **behaves**: guarantees, ordering, lifetimes, edge cases and error handling. Start at [`index.html`](index.html).

- No build step, no JavaScript, no external fonts or scripts. Everything is plain HTML plus `assets/style.css`.
- Works opened straight from disk (`file://`) and on GitHub Pages (serve the `docs/` folder from the default branch). All links are relative.
- Light and dark themes follow the reader's system setting (`prefers-color-scheme`).

## Current baseline

The pages describe `master`. PRs #26–#34 have all merged; their `PR #nn` badges stay as history, pointing readers at the PR that introduced or changed a behaviour. Every page's footer says "Describes master through PR #34."; update it on every page (and in `_template.html`) when a later PR brings the pages up to date.

## Pages

| File | Covers |
|---|---|
| `index.html` | Overview, module list, architecture map, ownership, frame summary, cross-cutting rules |
| `application.html` | Init/Run/Shutdown, frame order, fixed step, Quit, health, headless mode |
| `gameobjects-components.html` | GameObject/Component lifecycle, hierarchy, destroy, component lookup and removal |
| `transforms.html` | `Positionable` local/world transforms, dirty propagation, physics notification |
| `scenes.html` | `Scene` and `SceneManager`: storage, loading, switching, teardown |
| `coroutines.html` | Coroutine scheduling, waits, stop rules, re-entrancy |
| `physics.html` | Backends, bodies, colliders, events, raycasts, mass properties |
| `input.html` | Input system, action maps, bindings, value combination |
| `audio.html` | Audio system, sources, one-shots, mixer groups, shutdown |
| `text.html` | Font backends, SDF atlas, UTF-8 and text layout, the `Font` asset and default font, the `TextRenderer` component |
| `ui.html` | Screen-space UI: canvases, `RectTransform` layout, `Image`, the UI pass, UI picking |
| `resources.html` | Resource paths, `ResourceLoader`, `Resources` |
| `serialization.html` | Scene/component JSON, references, failure handling |
| `math.html` | Math types, SIMD tiers, correctness guarantees |
| `scripting-lua.html` | `LuaComponent`, fields, checked handles, subscriptions, `engine-api.lua` |
| `logging-and-editor.html` | Logger, engine health, the editor host |
| `testing.html` | Test layout, CI, conventions |

## Adding or changing a page

1. Copy `_template.html` to `<topic>.html`. It contains the full page skeleton: head, sidebar, "On this page" box, callouts, footer. Delete its "Authoring template" callout and comment in the copy. The template itself is not linked from the site (and Jekyll is disabled with `.nojekyll`, so GitHub Pages serves it as-is).
2. Set `<title>` to `<Page title> · N2Engine docs` and add `aria-current="page"` to the page's own sidebar link.
3. Add the new page to the sidebar of **every** page. There is no JavaScript to generate it, so the sidebar is repeated verbatim in each file. Keep the order and grouping identical everywhere.
4. Add the page to the table above and, if it is a subsystem, a card on `index.html`.

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
