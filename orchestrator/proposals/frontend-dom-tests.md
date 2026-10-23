# Proposal: DOM tests for the editor frontend (N2Engine-Electron-Frontend)

Research only; nothing was changed in the repo. "Verified" = run in a scratch dir under Node v22.23.2 with node --test.

## 1. Current setup (read from the repo)
- Runner: `node --test "dist/test/**/*.test.js"` after `npm run build` (package.json). Tests are src/test/*.test.ts, compiled by tsc (CommonJS) to dist. No tsx / strip-types.
- Components are NOT compiled by tsc: tsconfig.json excludes `src/renderer/**/*.tsx`; tsconfig.renderer.json only type checks them (`jsx: react-jsx`, `jsxImportSource: preact`, DOM lib). esbuild (scripts/bundle.js) bundles them (jsx automatic, preact) for the page.
- Framework is **Preact 11 + @preact/signals, not React**. So "@testing-library/react" is wrong; use @testing-library/preact.
- Seam already exists: components never touch `window.api`. index.tsx builds `window.engine/host/project/remote/play` into `EditorStore(api)` etc. and provides them via `AppContext` (src/renderer/ui.tsx `useApp()`); welcome.tsx only calls `useApp().store`. src/test/store.test.ts already builds a fake `StoreApi`, so a test can construct a real `EditorStore` over a fake api, or pass a stub `{ store } as AppState`. No production change needed.
- Note: the checkout's node_modules had no preact installed (stale install); run `npm ci` before testing.

## 2. Options (all devDependencies only)
| Option | Adds | Verdict |
|---|---|---|
| A. jsdom + @testing-library/preact (+ dom, user-event) | jsdom 30.1.2 (MIT, 7 MB), @testing-library/preact 3.2.4, /dom 10.4.2, /user-event 14.6.8 (all MIT, ~7 MB) | **Recommended.** Verified with Preact 11.0.0 + signals 2.11.3: render, fireEvent, userEvent typing, signal updates, `findBy`/`waitFor`. |
| B. happy-dom + same RTL | happy-dom 21.0.1 (MIT, 16 MB, Node >=20) | Verified: the same welcome test passes (1.5 s vs 2.1 s). Faster but less strict about spec behaviour, and a fast-moving security history (VM escape fixes); no gain that matters at our size. |
| C. jsdom + preact/test-utils `act` only, no RTL | jsdom only | Not run (assumed to work, `act` is exercised by RTL anyway). Saves ~7 MB but hand-rolls queries/events; more brittle tests. |
- Engines: jsdom 30 needs Node ^22.22.2 || ^24.15 || >=26, but package.json says `node >=21`. Either bump engines (Electron 39 ships Node 22; local is 22.23.2) or pin `jsdom@^26` (Node >=18; assumed, not run). Recommend bumping engines to `>=22.22`. RTL/user-event/happy-dom have no conflicting engines.
- Licences: all MIT. Maintenance: all active. @testing-library/preact's peer range is `preact >=10`; works on 11 (verified).
- Test time: jsdom import ~1.3 s once per test file process; each test ms. Whole welcome file 2.1 s total, dominated by startup. node --test runs files in parallel, so about +1-2 s on the suite. Not a concern.

## 3. How TSX gets compiled under node:test (verified)
Reuse esbuild (already a devDependency), no new loader: a new `scripts/bundle-tests.js` (or extra step in bundle.js) builds `src/test-dom/*.test.tsx` to `dist/test-dom/` with `bundle: true, packages: "external", platform: "node", format: "cjs", jsx: "automatic", jsxImportSource: "preact"` (renderer files get inlined, npm packages stay external). 0.15 s build. Then `node --test "dist/test/**/*.test.js" "dist/test-dom/**/*.test.js"`. This keeps tsc untouched and the DOM tests off the Node-only tsconfig. Add a `tsconfig.test-dom.json` (extends renderer, adds `types: ["node"]`) to the `typecheck` script so the tests are type checked.
- CSS/assets: no `.css` or image imports exist anywhere in src/renderer (grep), and index.html carries styles; nothing to stub. If one appears later, esbuild `loader: {".css": "empty"}`.
- DOM globals: a `src/test-dom/setup.ts` first-import creates `new JSDOM(..., {url:"http://localhost/"})` and copies window/document/etc onto globalThis (verified; ~10 lines), cleanup in `afterEach`.

## 4. First slice (3-5 tests; each exercised or modelled in the spike)
1. **Direct-remote form** (welcome.tsx DirectForm) - verified against the real file: unknown host shows "isn't trusted yet" + fingerprint, `connectDirect` NOT called until "Trust this fingerprint and connect"; call carries the shown fingerprint; unchecking TLS for 10.0.0.5 shows "Plaintext is only allowed to this computer". To add: `changed` prompt shows Pinned/Now presented and a Forget pin button that calls `forgetPin`; Forget pin button is visible with TLS on without any prompt; typed token is cleared after connect (`type=password` value "").
2. **Pass-list editor** (FieldEditor with `typeName "TextPass[]"`, PassListEditor itself is not exported) - verified: Move down commits reordered array, first Move up disabled, Remove, Add pass commit whole arrays; disabled hides buttons; non-list value falls back to JSON editor.
3. **Store-driven text** - Welcome Recent Projects list / Remove button, Open Project disabled when `hostLocation.problem` set, using a real `EditorStore` over the existing fake StoreApi (store.test.ts pattern) so signals drive the DOM.
4. **Assets panel / hierarchy** (second PR): context-menu and rename prompt flows, tree expand/collapse, with AssetsState/HierarchyState over fake engine.
Injection: a helper `renderWithApp(ui, overrides)` in src/test-dom/helpers.tsx wraps `AppContext.Provider` with a stub or a real store built from a `fakeStoreApi()` extracted from store.test.ts into src/test/ (shared). Stubs should be typed `Partial<AppState>` with a Proxy that throws on unexpected members, so a component reaching for state the test did not set fails loudly (my first spike run crashed on a missing `recentRemotes`, which is the failure mode).

## 5. Risks / jsdom gaps
- viewport-panel.tsx: canvas 2D/WebGL, ResizeObserver, pointer capture are absent; keep it out of DOM tests (viewport math/controller are already unit-tested as pure code). Stub `ResizeObserver` and `canvas.getContext` only if a panel that embeds it must mount.
- Drag and drop: jsdom has no DataTransfer; drag-types.ts parse logic is already pure; test components via `fireEvent.drop` with a hand-made `dataTransfer` object, or skip.
- No layout: `getBoundingClientRect` is zeros; popover positioning/splitters can't be asserted.
- Focus/IME, `:focus-visible`, `scrollIntoView` (not implemented in jsdom; stub it).
- Preact specifics: state updates are batched async; use `findBy`/`waitFor` or RTL's `act` wrapper; signals updates outside `act` need `await`.
- Version coupling: jsdom engine floor above repo's `engines`.
- No CI on this repo, so the guard is `npm test` locally only.

## 6. PR plan
PR 1 (small, ~250 lines): add devDependencies `jsdom@^30`, `@testing-library/preact@^3.2.4`, `@testing-library/dom@^10.4.2`, `@testing-library/user-event@^14.6.8` (explicit `/dom` because it is a peer of user-event); bump `engines.node` to `>=22.22`; scripts/bundle-tests.js, tsconfig.test-dom.json, `npm test` / `typecheck` wiring; src/test-dom/{setup.ts,helpers.tsx} and tests for slice items 1 and 2; README testing note (what DOM tests cover, jsdom limits).
PR 2: slice item 3 plus extracting `fakeStoreApi()` for sharing. PR 3: assets/hierarchy. Do not add `@types/jsdom` unless setup.ts typing needs it (jsdom ships no types; the spike's esbuild ignores types, so typecheck of setup.ts is **unverified**; expect to add `@types/jsdom` devDependency or `declare module`).

Spike files (throwaway): C:/Users/NICHOL~1/AppData/Local/Temp/claude/C--Method-Notes-Obsidian-N2Engine/13501c5a-9e29-4a9f-b99e-a85ce3bf9293/scratchpad/dt
