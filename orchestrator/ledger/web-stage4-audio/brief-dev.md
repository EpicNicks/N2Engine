# Task web-stage4-audio: Web build stage 4: audio through Emscripten OpenAL / WebAudio, started by a user gesture (issue #155, stage 4)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage4-audio.

## Issue / plan
Issue #155 stage 4 (read the issue body with: bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body, from inside the repo) and the merged stages 0-3: docs/web-build.html (read ALL of it: 'The window and audio stand-ins', the test-exclusion table, the player section, the loader), cmake/N2EngineWeb.cmake, external/web-stubs/ (openal_stub.c and its CMake), web/ (player/WebPlayer.cpp, WebInputMap, n2-loader.js, shell.html, budgets.json), the native audio code (engine/src/audio/*, engine/include/engine/audio/*) and tests/engine/audio/*. Decisions already taken (do NOT revisit): static files only on ANY host with NO special headers; single-threaded baseline; browsers require a USER GESTURE before audio starts; Emscripten's OpenAL (WebAudio) is the intended backend; native builds must be completely unchanged. GOAL: the web player plays the engine's AudioSource/AudioListener sounds in a browser. SCOPE: (1) INVESTIGATE FIRST, record in docs and the PR: which OpenAL entry points and extensions the engine actually calls (grep engine/ for al*/alc* uses, including loopback / alcLoopbackOpenDeviceSOFT, alcRenderSamplesSOFT, buffer formats, AL_SOFT_* extensions, EFX if any, the distance model, listener and source parameters, streaming buffers queue/unqueue) versus what Emscripten's built-in OpenAL port (-lopenal, library_openal.js) supports. Choose per the findings: (a) link Emscripten's port where the used subset is covered (preferred); (b) where a used call is missing or unsupported (the stage 1 docs already note the port lacks OpenAL Soft's loopback extension), make the ENGINE degrade explicitly on web (the audio system reports which capability is absent, plays what it can) rather than adding native behaviour changes; or (c) a small WebAudio shim only if (a) is infeasible, justified in a deviation. (2) REPLACE the stage 1 stand-in on the web: openal_stub.c must no longer be what the real player links, BUT headless node tests (stage 1 harness under node, which has no WebAudio) must keep working: keep a stub/null path for node (select by an explicit build/runtime flag or by the module environment: state the mechanism) and keep the existing web test exclusions valid, adjusting the exclusion table only where a test can now truly run under node. A no-device or failed-context case (no WebAudio, autoplay blocked, context creation fails) must leave the audio system disabled with one clear warning, exactly like a machine with no sound card, never a crash or a hang. (3) USER GESTURE: the AudioContext must be created/resumed from a user gesture. Implement in the page loader/shell (web/n2-loader.js, shell.html, the player): unlock on the first pointerdown/keydown/touchend (and a visible 'click to enable sound' affordance only if the shell already has a place for status text; no redesign), resume the context, and make the engine side tolerate audio starting late (sounds requested before the unlock are either queued briefly or dropped with documented behaviour: choose, test where possible, document). Also handle the tab becoming hidden/visible (suspend/resume) if the port does not. (4) Volume/mute: if the engine has a master volume API, make sure it reaches the web backend; add a JS-side mute hook on the player module only if trivial (note it in docs; no new public engine API). (5) SIZE: re-run the budgets; the audio libs add size; update web/budgets.json deliberately with the measured numbers and reasoning in the PR (do not loosen budgets without recording why). (6) TESTS: node tests under the existing web harness that can run without WebAudio: a mock of the Emscripten OpenAL surface or the stand-in path proving the audio system degrades cleanly and the unlock/queue behaviour in the JS loader (mock AudioContext: resume called once on the first gesture, listeners removed, failure tolerated), and keep all existing web and native suites green. (7) DOCS: docs/web-build.html (stage 4 section: what is supported, what is not, the user-gesture rule for game authors (a game should start sound after an input; what happens to earlier sounds), size impact, the node limitation), docs/audio.html web note, docs/testing.html, and the manual browser checklist docs/web-checklist.html with concrete audio steps (sound plays after the first click; no sound and no errors before it; mute/visibility behaviour; looping and streaming sources; many simultaneous sources; Safari/Firefox/Chrome notes). Verify every docs sentence against the CODE. Nothing builds locally; the Emscripten CI job and node tests are the test; expect fix rounds. List as UNVERIFIED everything needing a real browser (audible output, autoplay policy behaviour per browser, latency, Safari quirks) so the user knows what to check; do not claim it works in a browser.

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^external/
- ^web/
- ^scripts/
- ^engine/
- ^tests/
- ^docs/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Web player links a real WebAudio-backed OpenAL (Emscripten port or justified shim); the node stub path remains for headless tests
- AudioContext is unlocked by a user gesture; no-device/blocked/failed context degrades to disabled audio with one warning
- Used OpenAL subset investigated and documented; missing capabilities degrade explicitly; native builds unchanged
- Node tests for degrade path and loader unlock; budgets updated deliberately; all 10 checks green
- Docs and manual browser checklist updated; browser-only items listed as unverified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: native builds/tests unchanged (diff of native paths), the web player really links the web backend while node tests keep the stub, nothing calls audio before the gesture in a way that throws, failure paths never crash/hang, loader unlock listeners cleaned up and idempotent, size budgets changed only with recorded reasons, docs true vs code, checklist concrete. Unverified: real browsers.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage4-audio` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage4-audio` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage4-audio/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage4-audio --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage4-audio/dev.json: {"pr":<number>,"branch":"feat-web-stage4-audio","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage4-audio` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
