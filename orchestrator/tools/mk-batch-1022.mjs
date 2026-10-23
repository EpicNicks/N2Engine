import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const PR = 'C:/Method Notes Obsidian/orchestrator/proposals/'
const NET = PR + 'netcode-runtime-design.md'
const WG = PR + 'webgpu-backend-design.md'
const LX = PR + 'lua-editor-extensions.md'
const UI = PR + 'electron-ui-roadmap.md'
const MCP = PR + 'mcp-engine-design.md'
const SEC = PR + 'security-design-lua-editmode-and-preauth-slots.md'
const AUD = PR + 'editor-host-security-audit-2026-10-15.md'
const COMMON = ' Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.'
const FECOMMON = ' Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test (Node + DOM tests) must pass before you push; CI has 3 checks. Append ONE README.md section for the feature (other PRs also append: keep yours self-contained). Follow the existing panel/keymap/prefs registries (F1/F2) and lane file boundaries in the roadmap section 16. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.'
const FEPATHS = ['^package\\.json$', '^package-lock\\.json$', '^README\\.md$', '^src/', '^scripts/']
const items = []
const add = (o) => items.push({ closes: null, allowProtocol: false, gate: null, approved: true, requiresDocs: true, repo: 'engine', ...o })

add({
  name: 'net-a4-lows',
  branch: 'fix-net-a4-lows',
  title: 'Netcode A4 follow-ups: drain the socket before the idle-peer sweep, DualStack() only for wildcard IPv6 binds, ITransport header text brought up to date',
  issue: `Follow-up of the merged A4 PR #203 (native UdpTransport). The user accepted three LOW review findings as a follow-up item; verify each against the CODE on master before changing it: (1) engine/src/network/n2net/UdpTransport.cpp (~line 832): the idle-peer sweep runs BEFORE the socket is drained in Poll, so a stall between Polls longer than peerIdleTimeoutMs drops every peer although their datagrams sit in the receive buffer: drain first (bounded as today), then sweep, so a peer whose datagram was just received is not timed out; add a test that stalls longer than the idle timeout with datagrams queued and asserts the peer survives (and a peer that sent nothing is still swept); (2) UdpTransport.cpp (~line 613): DualStack() returns true for any IPv6 bind whose IPV6_V6ONLY=0 was accepted, including a specific address such as ::1 that can never reach IPv4, so Connect to an IPv4 address succeeds and every Send fails silently: DualStack() is true only for the IPv6 wildcard (::) bind; Connect to an IPv4 peer on a non-dual-stack IPv6 socket fails with a clear error; test both (skip cleanly when IPv6 is unavailable, as the existing tests do); (3) engine/include/engine/network/n2net/Transport.hpp (~line 52): the ITransport header comment still says the library has no sockets / native UDP is later work and that Disconnect makes the other side see PeerDisconnected; rewrite it to describe the current contract (UdpTransport exists, native only; the Disconnect semantics UdpTransport actually has, which the user accepted as a deviation in #203: read the A4 ledger C:/Method Notes Obsidian/orchestrator/ledger/net-a4-udp-transport/pr-body.md for the accepted wording). Update docs/netcode.html where it describes these behaviours.${COMMON}`,
  allowedPaths: ['^engine/', '^tests/', '^docs/'],
  acceptance: ['Poll drains the socket before the idle sweep; stall test passes', 'DualStack() only for the wildcard IPv6 bind; IPv4 connect on a specific IPv6 bind is refused clearly; tested', 'Transport.hpp comment matches the code', 'All checks green; docs verified'],
  reviewer: 'sonnet',
  notes: 'Reviewer: drain bound kept (no unbounded loop), sweep order, the IPv6 test skips cleanly, header text true, no flaky timing (deadline polling).',
})

add({
  name: 'tests-eval-order-docs-lows',
  branch: 'fix-tests-eval-order-docs-lows',
  title: 'Split multiple RNG calls out of single argument lists in two older test files; drop the hard-coded per-executable test counts from docs/web-build.html Stage 1',
  issue: `Two accepted lows. (1) EVALUATION ORDER: C++ leaves the evaluation order of function arguments unspecified, so a test that calls a stateful RNG two or three times in ONE argument list builds a different value sequence on MSVC and Clang (this already broke a PhysX determinism test once). Found on master: tests/engine/network/n2net/N2NetFuzzTests.cpp around lines 88 and 101, tests/engine/picking/PickAccelerationTests.cpp around line 59. Read both files, rewrite every such call so each RNG draw is its own statement in a fixed order (named locals), keeping what the tests check; then grep the WHOLE tests/ tree (and engine/ for good measure) for any other argument list or operator expression with two or more calls to the same stateful generator (rng(), dist(rng), Next(), Random*, std::uniform_*): fix every one found and list them in the PR body. (2) docs/web-build.html: the Stage 1 paragraph still lists hard-coded per-executable test counts that go stale with every PR: replace them with a description that does not hard-code counts (verify what the web leg actually runs from .github/workflows/tests.yml and the web scripts). Do not change behaviour anywhere else.${COMMON}`,
  allowedPaths: ['^tests/', '^engine/', '^docs/'],
  acceptance: ['No argument list or operator expression in tests/ calls a stateful RNG more than once', 'docs/web-build.html Stage 1 has no hard-coded test counts', 'All checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: grep yourself for remaining multi-draw expressions; tests still check the same properties; docs true.',
})

add({
  name: 'net-a5-handshake-aead',
  branch: 'feat-net-a5-handshake-aead',
  title: 'Netcode A5: connection handshake with a stateless challenge cookie, per-connection keys and AEAD-sealed packets on Mbed TLS primitives, join tokens, and the shared security policy library moved out of editor-server',
  issue: `Netcode design ${NET} (N1-N15 ACCEPTED): read sections 1.3 (threat model), 2.1 (2.1.1, 2.1.3 FULLY), 2.2, the A5 row of 7.2 and the risks; the editor security designs ${SEC} and ${AUD} (PreAuthSlots, TokenTable hashing, rate limits, fail-closed bind) as the precedent. MERGED prerequisites: A1 presets/settings (#187), A2 simulation clock, A3 n2net core (channels, ITransport, LoopbackTransport), A4 native UdpTransport (#203: fail-closed non-loopback bind unless --insecure-bind; its API takes an explicit 'encryption available' parameter that is false today: A5 makes it true when the secure layer is on). Mbed TLS 3.6.7 is vendored in external/mbedtls but only built for the editor host behind N2ENGINE_EDITOR_TLS. SCOPE (the design recommends our own AEAD packet layer with an ECDHE handshake using Mbed TLS primitives, reviewed against the netcode.io standard: follow it; record any departure as a deviation): (1) BUILD: an Mbed TLS crypto target (crypto only: no X.509/TLS needed) built for the native game/server builds on every native leg; the web build skips it (the browser supplies transport security later, B10) and must still compile; (2) SHARED POLICY LIB: move TokenBucket, RateLimiter, PreAuthSlots and TokenTable from editor-server into an engine-side library both use, with NO behaviour change for the editor host (its tests must pass unchanged apart from include paths); (3) HANDSHAKE (a layer between ITransport and the A3 channels): client sends a PADDED connect request (request size >= reply size: no amplification) carrying a protocol id/version and a join-token proof; the server answers with a STATELESS challenge cookie (HMAC over source address + coarse time + a per-process secret, short expiry) and allocates NOTHING; the client echoes the cookie; only then does the server consume a PreAuthSlots slot and complete the key exchange (ECDHE, e.g. X25519 if Mbed TLS 3.6 provides it in the vendored config, else P-256: verify in the headers); session keys via HKDF per direction; (4) AEAD packets (ChaCha20-Poly1305 or AES-GCM: pick what the vendored config has, verify) with nonce = per-direction packet sequence, a replay window, packets failing auth dropped silently and counted; (5) JOIN TOKENS: v1 static tokens file with roles (player, spectator, admin) hashed like the editor's TokenTable (SHA-256), constant-time compare, never logged (redaction like the editor log lines); (6) the A4 fail-closed rule becomes: non-loopback bind allowed when the secure layer is on; --insecure-bind still required otherwise; (7) TESTS: handshake success, wrong token refused, cookie forged/expired/from another address refused, no slot allocated before the cookie echo (slot count assertion), the AMPLIFICATION BOUND (server reply bytes <= request bytes for every pre-auth message), replay of a sealed packet dropped, tampered packet dropped, nonce reuse impossible by construction, rate limit on connect floods, loopback end to end with A3 channels over UdpTransport plus the secure layer; bounded run time; (8) DOCS: docs/netcode.html (handshake diagram in text, threat model coverage, what is NOT covered: no forward secrecy beyond ECDHE per session if that holds, no certificate identity of the server: say exactly what a client can and cannot trust), docs/testing.html. THIS ITEM NEEDS THE USER'S SECURITY-REVIEW ACCEPT AT MERGE: put a 'Security review' section in the PR body listing every cryptographic choice, the exact primitives and parameters, what is and is not authenticated, and the known limits.${COMMON}`,
  allowedPaths: ['^engine/', '^editor-server/', '^tests/', '^docs/', '^CMakeLists\\.txt$', '^cmake/', '^external/mbedtls/', '^\\.github/workflows/tests\\.yml$'],
  acceptance: ['Stateless cookie handshake; no server state before the echo; amplification bound tested', 'ECDHE + HKDF per-direction keys; AEAD with sequence nonces and a replay window; tamper/replay tests', 'Join tokens hashed, constant-time compare, never logged', 'Policy classes moved with no editor-host behaviour change', 'Web still compiles; all checks green; docs verified', 'PR body has a Security review section for the user'],
  reviewer: 'opus',
  notes: 'Reviewer (security): nonce uniqueness per key and direction, replay window correctness, key separation per direction, cookie binding (address, time, secret rotation), constant-time compares, no allocation/state before cookie, amplification, token redaction in logs, error paths that leak timing or oracle info, Mbed TLS API misuse (return codes checked, contexts freed), the editor host unchanged after the move, docs claims exact. Mediums count.',
})

add({
  name: 'webgpu-g4-emscripten',
  branch: 'feat-webgpu-g4-emscripten',
  title: 'WebGPU G4: Emscripten build of the WebGPU backend with emdawnwebgpu, --renderer argument and RenderBackend::WEBGPU, windowless WebGPU path in Window, backend compiled into the web player (still started on software), budgets re-measured',
  issue: `WebGPU design ${WG} (W1-W10 ACCEPTED): read sections 2, 3, 4.4, 5 and the G4 row of 7 FULLY plus 8 risks. MERGED: G0 shared GPU pieces, G1 WGSL + validation, G2 native harness (Dawn on lavapipe, non-required 'webgpu (Dawn, lavapipe)' job), G3a resources + unlit, G3b lit/blend/text; the web build stages 4/5 and the size budgets (web/budgets.json; NOTE the Jolt J3 PR #205 is about to raise web/budgets.json too: merge origin/master before your final push and re-measure on top of it). The WebGpuRenderer must compile for Emscripten against webgpu.h only (the emdawnwebgpu port, pinned): no Dawn-native-only APIs in the shared code path (guard the native-only bits: e.g. synchronous readback natively vs the async path on the web). SCOPE exactly the G4 row: (1) the emdawnwebgpu port in the web build (pinned, documented; verify the flag/port name for the Emscripten version CI uses), (2) a --renderer argument (software|webgpu, and the native values that exist) and RenderBackend::WEBGPU in the enum with selection logic, (3) the windowless WebGPU path in Window for the web, (4) the backend compiled into the web player but the player still STARTS ON SOFTWARE (canvas attach, present and loader selection are G5, not here), (5) web link in all three profiles, the node smoke still green on software, (6) budgets re-measured and raised only by what the numbers show, with the before/after numbers in the PR body, (7) docs: docs/web-build.html (the WebGPU build part, what is not wired yet), docs/rendering docs where --renderer is described.${COMMON}`,
  allowedPaths: ['^engine/', '^renderer/', '^tests/', '^docs/', '^web/', '^CMakeLists\\.txt$', '^cmake/', '^\\.github/workflows/tests\\.yml$', '^player/', '^editor-server/'],
  acceptance: ['Web player links the WebGPU backend in all three profiles via emdawnwebgpu (pinned)', '--renderer argument and RenderBackend::WEBGPU exist; web still starts on software; node smoke green', 'Budgets re-measured with numbers in the PR body', 'Native WebGPU job still green; all checks green; docs verified'],
  reviewer: 'opus',
  notes: 'Reviewer: no native-only Dawn API leaks into the web path, the async readback path, emdawnwebgpu pin, budgets raised only by measured amounts, the player does not start on WebGPU yet, --renderer validation and errors, docs true.',
})

add({
  name: 'lua-ex5-inspector-actions',
  branch: 'feat-lua-ex5-inspector-actions',
  title: 'Lua editor extensions E-X5 (engine): inspector sections declared by extensions and InvokeInspectorAction (0x83), protocol minor 1.16.0',
  issue: `Lua editor extensions design ${LX} (D1-D12 APPROVED): read 4.1-4.4, 6.3, 6.4, 7.1 (the 0x83 row and the GetEditorUi inspectorSections shape) and the E-X5 row of 9 FULLY. MERGED: E-X1 sandbox profiles, E-X2 ExtensionHost (discovery, registry, GetEditorUi, protocol 1.14.0), E-X3 Editor.Scene API + InvokeMenuItem 0x82 with one edit group per invocation, all-or-nothing multi-command calls, rollback, extensionOutput, error codes staleUiRevision/editGroupOpen (protocol 1.15.0). Read editor-server's extension host code, InvokeMenuItem's implementation and tests, protocol.json and the generators FIRST: InvokeInspectorAction must reuse the same invocation machinery (edit group, rollback, budgets, nested dispatch with the invoker's role, audit line, staleUiRevision, editGroupOpen, faulted state) rather than duplicating it. SCOPE (engine side only; the frontend sync is a separate later PR): (1) the declaration API in extension files for inspector sections per the design (componentType, label, fields as FieldSchema, buttons with optional forms) captured at load, validated, and returned in GetEditorUi.inspectorSections (check what E-X2 already returns there and extend it); (2) InvokeInspectorAction 0x83 {componentId, actionId, params, uiRevision} -> ExtensionResult, ctx carries componentId and entityId, Write class, IsEditOnlyCommand (a play host refuses), classification table entry so the 'every command classified' test passes; minRole honoured like menu items; (3) protocol minor 1.16.0: protocol.json, regenerated clients via editor-server/protocol/generators, test-vectors.json, docs/logging-and-editor.html protocol section; (4) TESTS mirroring InvokeMenuItem's: one invocation = one undo step, failure rolls back, viewer forbidden, read-only refused, stale uiRevision refused, component id that does not exist or does not match the section's componentType refused, budget trip leaves the host answering, nested write gets the invoker's role, audit line; (5) docs: docs/editor.html and docs/scripting-lua.html (extension API: inspector sections).${COMMON}`,
  allowProtocol: true,
  allowedPaths: ['^engine/', '^editor-server/', '^tests/', '^docs/', '^CMakeLists\\.txt$'],
  acceptance: ['Inspector sections declared, validated, returned by GetEditorUi', 'InvokeInspectorAction 0x83 reuses the InvokeMenuItem machinery; classified Write and edit-only', 'Protocol 1.16.0 with regenerated clients and vectors', 'Tests listed in the brief pass; all checks green; docs verified'],
  reviewer: 'opus',
  notes: 'Reviewer (security/protocol): role checks, componentId/componentType binding, no duplicate invocation path that skips budgets or the edit group, classification, generators re-run (no hand edits), vectors, docs true. Mediums count.',
})

add({
  name: 'fe-fe9b-layers-matrix',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'feat-fe9b-layers-matrix',
  title: 'Electron UI FE9b: layer collision matrix editor (32x32 checkbox grid) in the project settings Layers section',
  issue: `Electron UI roadmap ${UI} (U1-U21 ACCEPTED): section 5 (project settings; FE9b row: the layers collision matrix as a 32x32 checkbox grid in a second PR) and section 16 (panels lane: FE9 -> FE9b -> FE15). MERGED: FE9 project settings panel (frontend #43: section registry, typed tables in project-settings-schema.ts, Layers section with 32 names and a READ-ONLY note about the collision matrix). Read the FE9 code, its README section, and in the engine repo the project settings format for layers and the collision matrix (engine physics layer matrix and how SetProjectSettings validates it: find it in the engine code and docs/physics.html / docs/project-settings docs) FIRST: the frontend must send exactly the shape the host validates. SCOPE: replace the read-only note with an editable symmetric matrix (toggling (a,b) toggles (b,a)), rows/columns labelled with layer names (unnamed layers collapsed by default with a show-all toggle), row/column select-all helpers, dirty tracking and Apply/Revert consistent with the other FE9 sections, admin/role gating the same as other settings writes, read-only when the host is read-only or a viewer; DOM tests (toggle symmetry, apply sends the right payload, revert, role gating, 32-layer bounds) and unit tests for the matrix model.${FECOMMON}`,
  allowedPaths: FEPATHS,
  acceptance: ['Editable symmetric 32x32 matrix in the Layers section sending exactly what the host validates', 'Apply/Revert/dirty and role gating like the other sections', 'Node + DOM tests pass; 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: payload matches the engine validation (read the engine code), symmetry, bit order, gating, tests real.',
})

add({
  name: 'fe-fe10-import-settings',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'feat-fe10-import-settings',
  title: 'Electron UI FE10: typed import settings editor for assets (previews come later with EN1)',
  issue: `Electron UI roadmap ${UI} (U1-U21 ACCEPTED): read section 6 (import settings UI and asset previews) FULLY and section 16 (assets lane: FE10 -> FE14). Previews/thumbnails need the engine EN1 minor (GetAssetPreview) which does NOT exist yet: NOT in this PR. Read the frontend assets panel code (assets-*.ts(x)), the existing import-settings handling, and in the engine repo the import settings commands and per-type option sets (assetimport/, protocol.json, docs) FIRST. SCOPE: section 6's FE10 part: typed per-asset-type import settings forms (fields, ranges, defaults taken from the engine's real option set; verify each against the engine code, cite where), validation before send, Apply (with the reimport the host performs) and Revert, multi-asset same-type editing only if the section asks for it, role/read-only gating; Node + DOM tests.${FECOMMON}`,
  allowedPaths: FEPATHS,
  acceptance: ['Typed import settings forms matching the engine option sets', 'Validation, Apply/Revert, gating', 'Node + DOM tests pass; 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: every field/range checked against the engine code, no preview code that calls non-existent commands, tests real.',
})

add({
  name: 'fe-fe12-marquee',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'feat-fe12-marquee',
  title: 'Electron UI FE12: marquee selection in the viewport',
  issue: `Electron UI roadmap ${UI} (U1-U21 ACCEPTED): section 11 (multi-select: the Marquee select (FE12) paragraph) FULLY and section 16 (viewport lane: FE3 -> FE4 -> FE12 -> FE13a -> FE13b; FE3/FE4 merged). Read the viewport files (viewport-*.ts(x)), the FE4 toolbar/modes, selection handling in the store, and GetEntityBounds in the engine protocol (cap 4096 ids per call) FIRST. SCOPE as specified: left-drag on empty space past the click slop (not on a gizmo handle) draws a rectangle; on release select objects whose projected world AABB intersects the rectangle (Alt: fully inside); Shift/Ctrl add/remove; bounds from GetEntityBounds for the hierarchy's nodes cached per scene revision and chunked above 4096; Escape cancels; no conflict with camera drags or gizmo drags; Node tests for the projection/intersection maths and DOM tests for the drag flow and modifiers.${FECOMMON}`,
  allowedPaths: FEPATHS,
  acceptance: ['Marquee select with intersect/inside and add/remove modifiers', 'Bounds cached per revision and chunked at 4096', 'Node + DOM tests pass; 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: projection maths (behind-camera points, clipping), chunking, no interference with gizmo/camera drags, cache invalidation.',
})

add({
  name: 'fe-fe11-multi-edit',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'feat-fe11-multi-edit',
  title: 'Electron UI FE11: inspector multi-edit for several selected objects in one edit group',
  issue: `Electron UI roadmap ${UI} (U1-U21 ACCEPTED): section 11 (the Inspector multi-edit (FE11) paragraph, including what it says about Lua components) FULLY and section 16 (inspector lane: FE5 -> FE11; FE5 merged). Read the inspector files (inspector-*.ts(x)), scene-state.ts, the edit-group handling in edit-controller.ts and the FE8 history panel FIRST. SCOPE as specified: with several objects selected show the common header fields (active, tag, layer, transform) and the component types present on ALL of them (matched by type, first of each type); mixed values show a dash; an edit sends SetComponentFields / SetEntityProperties / SetLocalTransform per object inside ONE edit group labelled 'Edit N objects' (one undo step; on any failure the group is aborted/rolled back the way the existing edit groups do it); Lua components per the section; role/read-only gating; Node + DOM tests (mixed display, one undo step, partial failure, selection change while editing).${FECOMMON}`,
  allowedPaths: FEPATHS,
  acceptance: ['Common fields and shared component types shown with mixed-value display', 'Edits applied to all objects in one edit group (one undo step), failure handled', 'Node + DOM tests pass; 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: edit-group open/close on every path (no group left open: the host refuses extension invokes while a group is open), partial failure, stale selection.',
})

add({
  name: 'mcp3-play-tools',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'feat-mcp3-play-tools',
  title: 'MCP3: n2-mcp play tools (--launch only), resources and subscriptions over the event pump, prompts',
  issue: `MCP design ${MCP} (M1-M12 ACCEPTED): read 3.1, 3.2 (the play rows), 3.6 resources, 3.7 prompts, 3.8 errors, 4 (security: confirmation tiers, budgets, data marking/scrubbing), 5.3, 7 testing and the MCP3 row of 9 FULLY. MERGED: MCP1 read tools (stdio server, --launch/--connect backends, budgets, scrubbing, golden schema file, fake host tests) and MCP2 write tools (sessions, journal, apply_edits, irreversible tier). Read src/mcp/ and src/main/play-session.ts FIRST and reuse their patterns. SCOPE exactly the MCP3 row: play tools available ONLY with --launch (absent from the tool list under --connect, with a test): start/stop through play-session.ts, control, input, observe, play_script with progress notifications and cancellation between ops; resources and subscriptions over the existing event pump (bounded, scrubbed and data-marked like tool results); prompts per 3.7; update the golden schema file; budgets and error mapping per the design; fake-host tests for every play flow, cancellation, subscription delivery and unsubscribe, prompt listing. README section for the new tools.${FECOMMON}`,
  allowedPaths: FEPATHS,
  acceptance: ['Play tools only under --launch; start/stop/control/input/observe/play_script with progress and cancellation', 'Resources + subscriptions over the event pump, bounded and scrubbed; prompts', 'Golden schema updated; fake-host tests pass; 3 CI checks green'],
  reviewer: 'opus',
  notes: 'Reviewer (security): play tools not reachable under --connect, scrubbing/data marking on resource payloads, subscription bounds (no unbounded queues), cancellation leaves the play session consistent, budgets.',
})

for (const it of items) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log(items.map(i => i.name).join('\n'))
