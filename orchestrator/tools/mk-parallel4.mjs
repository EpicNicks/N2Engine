import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const PR = 'C:/Method Notes Obsidian/orchestrator/proposals/'
const NET = PR + 'netcode-runtime-design.md'
const EX = PR + 'lua-editor-extensions.md'
const FE_PATHS = ['^engine-ref\\.txt$', '^package\\.json$', '^package-lock\\.json$', '^README\\.md$', '^src/', '^scripts/']

const a3 = {
  name: 'net-a3-n2net-core',
  repo: 'engine',
  branch: 'feat-net-a3-n2net-core',
  title: 'Netcode A3: n2net core: ITransport, in-memory SimTransport (latency/jitter/loss/reorder/duplication), packet header with sequence+ack bits, reliable-ordered and unreliable-sequenced channels, fragmentation',
  closes: null,
  issue: `Netcode design ${NET}: the user accepted ALL decisions N1-N15 on 2026-10-20 (own small UDP-style reliability layer, no new network dependency; browser WebSocket first; star topology through N2Server; etc.). Read sections 1-2 (especially 2.1 'Transport abstraction' incl. the packet and channel design, 2.2-2.4), 6 (testing strategy: the deterministic network simulator in CI), 7.2 (the staged plan: this is PR A3) and the 'Could not verify'/risks FULLY first. THIS ITEM IS A3 ONLY: a new, self-contained library with NO sockets, NO encryption, NO engine integration (A4 adds the native UDP transport, A5 the handshake/AEAD, B-track the replication). SCOPE: (1) a new static library target (e.g. engine/network/n2net with headers under engine/include/engine/network/n2net/ and sources under engine/src/network/n2net/: do not touch the existing NetworkPresets files in engine/src/network or the files of the in-flight netcode A1 PR (presets/validation/health): keep new files in a clearly separate directory and a separate CMake target linked into the engine/tests only where needed); compiles on Windows MSVC, Linux GCC 14/Clang, arm64 and the single-threaded web (Emscripten) build WITHOUT threads, no exceptions across API boundaries, no global state, no platform headers; (2) ITransport per design 2.1: datagram-oriented, non-blocking, polled on one thread: Send(peer, bytes), Poll(callback), MTU (max packet 1200 bytes), IsReliableStream() (true for stream transports: the upper layer skips its own resends but keeps sequencing), peer ids, connect/disconnect notifications as the interface needs them (keep it minimal and justify each method); (3) SimTransport: an in-memory implementation driven by an explicit virtual clock (no wall clock) and a seeded deterministic RNG (your own small portable PRNG, not std::random_device or distributions whose output differs across standard libraries), configurable per link and direction: one-way latency, jitter, loss (independent and burst), reordering, duplication, bandwidth cap and a queue limit; deterministic: the same seed and the same call sequence give byte-identical delivery on every platform (this is the CI network simulator for all later netcode tests); (4) the packet layer per 2.1: little-endian header = protocol id (32-bit), connection id, 16-bit sequence, ack + 32-bit ack bits (Gaffer-on-Games style, handle wraparound correctly), payload = channel blocks; reliable-ordered channel (resend on timeout/ack-gap with an RTT estimator, in-order delivery, flow/window limit) and unreliable-sequenced channel (newest wins, stale dropped); fragmentation and reassembly of large reliable messages (bounded memory per connection, a hard cap on message size and in-flight fragments, timeouts for incomplete messages); RTT/loss statistics per connection; strict, bounds-checked parsing of every untrusted byte (a malformed or hostile packet must never crash, over-read, allocate unboundedly or loop: this will face the internet): no exceptions, return error codes; (5) TESTS (engine unit tests on all legs including web and arm64; web_tests/engine_tests as appropriate: see how existing tests are registered and the web test-exclusion conventions in docs/web-build.html): sequence/ack wraparound, reliable delivery under 30% loss with reordering and duplication (exact in-order exactly-once delivery), unreliable newest-wins, fragmentation at sizes from 1 byte to the cap incl. boundary sizes and out-of-order fragments, caps/timeouts enforced, simulator determinism (golden delivery trace hashed and compared to a constant: identical across legs), a fuzz-style test feeding thousands of random and truncated packets (seeded, bounded run time) that must not crash or leak (use the allocator counters/size caps), RTT estimator sanity, two peers over SimTransport exchanging 10k messages. (6) DOCS: a new docs page docs/netcode.html (link it from the docs index/navigation like other pages) describing for now ONLY what exists: the transport interface, the packet format (byte layout table), channels, fragmentation limits, the simulator and how to use it in tests, security notes (untrusted input handling), and clearly what is NOT there yet (sockets A4, encryption A5, replication B-track); docs/testing.html entry; verify every sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds. Unverified to list: real network behaviour and performance on real links.`,
  allowedPaths: ['^engine/', '^tests/', '^docs/', '^CMakeLists\\.txt$', '^cmake/'],
  acceptance: [
    'n2net library: ITransport, deterministic SimTransport, packet header with ack bits, reliable-ordered and unreliable-sequenced channels, fragmentation; no sockets/encryption/engine integration',
    'Strict bounded parsing: fuzz test of random/truncated packets never crashes or allocates unboundedly; message and fragment caps enforced',
    'Simulator determinism proven by a golden trace across all legs incl. web and arm64',
    'All 10 checks green; docs/netcode.html describes only what exists, byte layout verified against code',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: parsing safety (every length/offset checked, no integer overflow, bounded allocation, reassembly DoS limits), ack-bit/sequence wraparound correctness, reliable channel exactly-once in-order under loss, simulator determinism across compilers (no unspecified float/PRNG/ordering behaviour, no std distributions), no wall-clock/threads/global state, web/arm64 portability (alignment, endianness helpers), tests meaningful, docs byte layout true, mediums count.',
}

const fe114 = {
  name: 'frontend-protocol-1-14',
  repo: 'frontend',
  branch: 'feat-protocol-1-14-extensions',
  title: 'Frontend: sync to protocol 1.14.0 (Lua editor extensions commands, event, capability and error codes): typed client support only, no UI',
  closes: null,
  issue: `Engine PR #189 (E-X2, merged) shipped protocol 1.14.0 (additive minor): commands ListEditorExtensions 0x80 (Read), GetEditorUi 0x81 (Read), ReloadEditorExtensions 0x84 (Admin) (0x82/0x83 reserved for later), an EditorEvent kind extensionsChanged {uiRevision} (EditorEvent gained an optional uiRevision), new errorCodes extensionsDisabled / extensionFailed / extensionFaulted, and ServerInfo.capabilities containing "editorExtensions" when the host supports them. Read the engine repo (C:/Method Notes Obsidian/N2Engine, READ-ONLY) editor-server/protocol/protocol.json, docs/editor.html and docs/logging-and-editor.html for the exact semantics and the design ${EX} section 7 (7.1 commands, 7.2 frontend) first. SCOPE (typed client support ONLY: the Tools menu, form dialog, trust prompt and admin dialog are the later E-X4 item; do NOT build UI): (1) bump engine-ref.txt to the current engine master commit and run \`npm run sync-protocol\` so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, NEVER hand-edited (the CI 'Generated protocol matches the pinned engine' check must stay green); (2) engine-client: typed wrappers listEditorExtensions(), getEditorUi(), reloadEditorExtensions() returning typed results (parse the JSON payloads defensively: unknown fields ignored, missing fields defaulted, bad shapes become a typed error, never a throw into the UI), expose serverInfo capabilities (a helper hasCapability('editorExtensions')), map the three new error codes to typed errors distinct from forbidden/readOnly/busy (follow how 1.11-1.13 coded errors such as forbidden/readOnly/busy are mapped), handle the extensionsChanged event and the optional uiRevision in the event model (store a uiRevision signal other code can subscribe to; no UI); (3) old hosts: against a 1.13 host (no capability, commands unknown) nothing changes: the helpers report 'unsupported' without sending the commands; (4) tests: node unit tests with the existing fake host/fixtures for the typed wrappers, the error-code mapping, event parsing, old-host behaviour and defensive parsing of malformed payloads; keep every existing test green; typecheck, build and tests run locally (npm run typecheck; npm test): run them before pushing; (5) README: the Protocol section (version 1.14.0 and what the client supports) and a short note that extension UI is not built yet; verify against the code. Rules: requiresDocs false (README only), no token in logs/storage, no UI changes (the in-flight F1 panel-registry item edits editor.tsx/panels.tsx/ui.tsx: stay out of those files).`,
  allowedPaths: FE_PATHS,
  acceptance: [
    'sync-protocol produces no diff against engine-ref.txt (CI check passes)',
    'Typed client wrappers for the three commands, error-code mapping, extensionsChanged event and capability helper; old hosts unaffected',
    'Defensive payload parsing tested; no UI added; typecheck, build, tests and frontend CI green'
  ],
  requiresDocs: false,
  allowProtocol: true,
  gate: null,
  approved: true,
  reviewer: 'sonnet',
  notes: 'Reviewer: no hand edits to generated files (sync-protocol output only), old-host path sends nothing, defensive parsing cannot throw into the UI, error codes mapped distinctly, no token persisted/logged, no UI files touched (editor.tsx/panels.tsx/ui.tsx), tests meaningful. Unverified: a live 1.14 host.',
}

const exf = {
  name: 'lua-ex2-followups',
  repo: 'engine',
  branch: 'fix-lua-ex2-followups',
  title: 'Lua extensions E-X2 follow-ups: total memory cap across extension states, overall reload time limit, logging under the registry mutex, int param min/max validation, and role/play tests that actually run the commands',
  closes: null,
  issue: `Follow-up to merged E-X2 (#189, editor-server/src/ExtensionHost.cpp and tests/editor-server/EditorExtensionCommandsTests.cpp; design ${EX}). The user accepted these as known notes and asked for a follow-up. Read the E-X2 docs in docs/editor.html and docs/scripting-lua.html first and the review notes in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex2-extension-host/review.json. FIX: (1) TOTAL MEMORY CAP: up to 64 files x 64 MiB each can load (4 GiB worst case, no total cap, documented): add a host-wide total cap across all extension states (a named constant, e.g. 256 MiB total, a per-state cap that shrinks or a load refusal for the file that would exceed it with a clear path-stripped reason 'extension memory budget exhausted', deterministic order = the existing sorted discovery order), tests (many small states stay under; a file that would exceed the total fails as DATA and the others still load) and docs. (2) RELOAD TIME LIMIT: a reload has no overall time limit (the command thread waits for files one after another, many seconds with many slow files): add an overall wall-clock budget for a Load/Reload (named constant; files not loaded in time fail as data with 'reload time budget exhausted' and are retried on the next reload, never cached as permanent failures), tests with a blocking hook, docs. (3) Load() logs from _entries after releasing _mutex, breaking the header's promise that the registry is guarded by the mutex: snapshot what is needed under the lock and log from the snapshot (or log under the lock if cheap), keep the header comment true. (4) An int param accepts min/max that are not whole numbers, and with no default takes min as its default so an Int field can get a fractional default: validate that int params have integer min/max/default (a bad shape fails the file as data with a path-stripped reason) and test. (5) The play-host test and the role tests only check the static tables: add tests that really run the commands through ExecuteCommand/the session path: a play host refuses ReloadEditorExtensions, a viewer and an editor may ListEditorExtensions/GetEditorUi but ReloadEditorExtensions is refused with forbidden for them and allowed for admin, a read-only host still lists, extensionsDisabled behaviour when the flag is off. Keep protocol and public behaviour otherwise unchanged (allowProtocol false: STOP and report if a protocol change is needed). Update docs (editor.html, scripting-lua.html, testing.html) and verify every sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds. Note: another in-flight engine item (MCP0a/0b) edits EditorServer.cpp: keep your changes inside ExtensionHost.* and the extension tests, plus docs.`,
  allowedPaths: ['^editor-server/', '^engine/', '^tests/', '^docs/', '^CMakeLists\\.txt$'],
  acceptance: [
    'Host-wide total memory cap across extension states with deterministic failure as data, tested',
    'Overall time limit for load/reload with retry on next reload, tested',
    'Logging no longer reads the registry outside the mutex; int param shape validation; tests that really execute role/play/flag cases',
    'All 10 checks green; docs verified against code',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: the caps cannot be bypassed (per-state plus total accounting correct when states fail/abandon/reload), budget-exhausted files are not cached as permanent failures, thread-safety of the logging change, int validation complete (NaN/inf/huge), tests really execute the commands, no change to EditorServer.cpp beyond what is needed, docs true, mediums count.',
}

const dw = {
  name: 'docs-web-stage5-lows',
  repo: 'engine',
  branch: 'docs-web-stage5-lows',
  title: 'Docs: correct the three accuracy lows left in docs/web-build.html after web stage 5 (test counts, cold-cache minutes, stage-6 threading statement)',
  closes: null,
  issue: `Docs-only item. Three known inaccuracies from the review of merged PR #184 (web stage 5) in docs/web-build.html, each to be verified against the CODE, CI configuration and measured CI logs before editing: (1) the 'The PhysX tests under node' section states engine_tests counts (1504 tests: 1490 passed, 14 skipped) that do not match the CI run at that head (1529 passed, 14 skipped, 1543 in all): do NOT hard-code counts that go stale: rewrite so the claim is about what is true and stable (e.g. which PhysX tests run under node and which are skipped and why) and, if counts are kept, say they are examples as of PR #184 and verify them from a current web CI log (use the wrapper: bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/actions/... for the latest master web job; be gentle with the API, no log downloads beyond one job); (2) the design-decision callout says the cold-cache PhysX source build takes 'about four minutes of one job' but the CI bullet above it measures 102 s of PhysX build, 122 s of configure and 'about 2 minutes more on a cold cache': read both, check the workflow's cache key and the CI timings, and make the two statements agree and be true; (3) the page says a threaded stage-6 artifact 'would not define the macro' (N2ENGINE_SINGLE_THREADED), as if that alone would bring the PhysX workers back: in fact the web PhysX libraries are built WITHOUT -pthread and neither the .pin stamp nor the CI cache key records threading, so a threaded artifact needs a separately built, separately keyed threaded PhysX: say that (verify in cmake/PhysXWeb.cmake, cmake/PhysXWebPin.cmake, scripts/web-build-physx.sh and the workflow). Keep edits minimal, re-read the HTML (well-formed), no other changes. Also check docs/testing.html and docs/physics.html for the same stale counts or claims and fix only verifiably wrong text.`,
  allowedPaths: ['^docs/'],
  acceptance: [
    'The three inaccuracies corrected and verified against code/CI; no stale hard-coded counts',
    'HTML well-formed; docs-only; all checks green',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'sonnet',
  notes: 'Reviewer: every changed sentence is true against the code and the CI configuration/logs, no stale numbers introduced, HTML well-formed, nothing else changed, mediums count.',
}
for (const it of [a3, fe114, exf, dw]) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log('ok')
