# Task web-stage3-packaging-budgets: Web build stage 3: precompressed outputs, size manifest and enforced size budgets in CI, lazy asset pack option, profiles built in CI, MIME-fallback loader (issue #155, stage 3)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage3-packaging-budgets.

## Issue / plan
Issue #155 stage 3 (read the issue body with `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body` from inside the repo) and the merged stages 0-2 (docs/web-build.html, cmake/N2EngineWeb.cmake, the web player package from stage 2, the stage 0 size-report script and web-size-report target, the web CI job). Decisions already taken (do NOT revisit): static files only, ANY host, NO special headers (no cross-origin isolation), relative URLs, MIME/compression-agnostic output, profiles dev/size/speed architected from the start, size budgets enforced in CI. SCOPE OF STAGE 3 (assets and packaging): (1) PRECOMPRESSED outputs: in the `size`/`speed` profile packages produce `.br` and `.gz` siblings of the wasm/js/data files (brotli/gzip via the toolchain or Python stdlib gzip + a pinned `brotli` Python module OR the `brotli` CLI if available in the emsdk/runner: pick what is robust and pinned) AND the plain files remain (the output must work on a host that does neither); document how to serve them (e.g. nginx/Apache/Netlify snippets for Content-Encoding) and that nothing is required; (2) a SIZE MANIFEST: a generated `manifest.json` in the package (file names, raw size, gzip size, brotli size, sha256) used by the loader for cache busting and by CI for budgets; (3) ENFORCED BUDGETS in CI: set concrete initial budgets (derive them from the measured sizes of the current stage 2 package with sensible headroom: measure in the CI job summary first by reading the job logs of recent web jobs; record the chosen numbers and reasoning in docs and the PR; budgets live in one place (a JSON/CMake file) and the script fails the web job when exceeded); (4) build the `size` and `speed` profiles in CI too (stage 0 only configured them): the size profile must build the player package; keep the CI time reasonable (state the added minutes; consider building size only on one job leg, but you may only add steps to the existing web job or one new job whose name you state; do NOT rename existing jobs; if you add a job it must follow the docs-only skip pattern); (5) the LOADER (index.html template/JS): if `WebAssembly.instantiateStreaming` fails because the host serves .wasm with the wrong MIME type, fall back to fetching an ArrayBuffer and `WebAssembly.instantiate` (Emscripten supports this: verify how stage 2's shell does it and make it robust and test it in the node smoke by a mock fetch that returns the wrong content type); progress display during download using the manifest sizes; load failure messages; (6) OPTIONAL lazy asset pack: let the player fetch its asset pack (.data) asynchronously with progress (Emscripten's preload via `--preload-file` is already fetched before start: the improvement is progress reporting and a clear error path; do NOT introduce a custom pack format unless small and tested); (7) docs/web-build.html updated: the budgets, manifest, precompressed outputs, loader behaviour, hosting recipes, what still is not done (audio, physics, WebGPU, threads); update docs/testing.html CI paragraph. Native builds must be completely unchanged. Nothing builds locally; the Emscripten CI job is the test, expect fix rounds. Unverified items: everything needing a real browser (progress display, MIME fallback in a real browser, Content-Encoding hosting recipes).

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^scripts/
- ^web/
- ^docs/
- ^engine/
- ^tests/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All 10 checks unchanged and green; native builds identical
- size/speed packages are built in CI; .br/.gz siblings and manifest.json are produced; plain files still work
- Size budgets are enforced by CI from one budget file; chosen numbers justified from measurement
- The loader falls back when .wasm is served with the wrong MIME type (node smoke with a mock fetch) and shows progress/errors; docs updated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: budgets reference real measured numbers and the budget file is the single source, CI job names unchanged (any new job follows the docs-only skip), precompressed files do not replace the plain ones, manifest hashes correct, the MIME fallback path really exercised by the smoke test, native builds untouched, docs true. Unverified: real-browser behaviour.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage3-packaging-budgets` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage3-packaging-budgets` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage3-packaging-budgets/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage3-packaging-budgets --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage3-packaging-budgets/dev.json: {"pr":<number>,"branch":"feat-web-stage3-packaging-budgets","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage3-packaging-budgets` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
