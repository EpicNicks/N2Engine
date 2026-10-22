import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const PR = 'C:/Method Notes Obsidian/orchestrator/proposals/'
const AUD = PR + 'editor-host-security-audit-2026-10-15.md'
const MCP = PR + 'mcp-engine-design.md'
const ROAD = PR + 'electron-ui-roadmap.md'

const a10 = {
  name: 'sec-audit10-viewer-event-filtering',
  repo: 'engine',
  branch: 'feat-sec-audit10-event-filtering',
  title: 'Security audit item 10 (F9), MCP0a: sensitivity tags on log lines and role-filtered PollEvents: viewers/editors no longer get audit, rate-limit or peer lines or host absolute paths',
  closes: null,
  issue: `Security audit item 10 / finding F9 of ${AUD} (read F9 and the recommended-order table row 10 first) and ${MCP} section on MCP0 (the reason it is a prerequisite: AI agents and shared hosts must not see other users' names, peer IPs and host paths). The user accepted ALL recommendations on 2026-10-20 (MCP0 before agents are pointed at shared hosts). TODAY: PollEvents returns the whole process log (users, roles, peer IPs, rate-limit lines, absolute paths) to every authenticated role including viewers. SCOPE: (1) SENSITIVITY TAGS on log records: extend the logger/EventRing record (find editor-server's event ring and the engine Logger hook that feeds it) with a sensitivity level (e.g. Public | Operator | Admin) assigned at the call site or by category: the audit: lines, 'Rate limit:' lines, peer=/address lines, token/identity/user lines, pre-auth/busy/takeover lines and anything that prints an absolute host path are Admin (or Operator) only; engine/game logs the project's scripts and components produce (Lua print/Debug.Log, component warnings) stay visible to every role. Prefer explicit tagging at the call sites you can find (grep Logger:: calls in editor-server/ and the audit/rate-limit/pre-auth code paths) with a safe default for untagged lines (decide: untagged engine lines are Public; untagged editor-server lines are Admin: justify and test); NEVER filter by string matching alone. (2) PollEvents (and any other command or event that returns log lines: grep for the log/event ring consumers, GetEngineHealth, the audio/health events) filters by the caller's role: admin sees everything; editor and viewer see only Public lines; a --no-token loopback host (no roles) keeps today's full visibility for the single local user (state this and test it); the ring must keep its eviction behaviour and sequence numbers consistent per client (a client must not see sequence gaps that leak the existence of hidden lines: either number per role view or document that gaps can occur: choose the safer one, test it). (3) REMOVE absolute host paths from anything non-admin roles still receive in events (paths in public lines become res://-relative or are stripped): coordinate with item 11 (error responses) only by keeping this item to the LOG/EVENT surface. (4) TESTS: PollEventsTests per role (admin full; editor and viewer filtered; no-token loopback full), a golden list of the exact audit/rate-limit/peer line prefixes being Admin-only, untagged-default behaviour, path stripping, sequence behaviour, concurrency of the ring unchanged; no protocol change (allowProtocol false: if the fix needs a protocol change STOP and report in needsUser). (5) DOCS: docs/logging-and-editor.html (what each role sees in the event stream: verify against the code), docs/editor.html roles text, docs/testing.html. Verify every docs sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds.`,
  allowedPaths: ['^editor-server/', '^engine/', '^tests/', '^docs/', '^CMakeLists\\.txt$'],
  acceptance: [
    'Log records carry a sensitivity tag; audit, rate-limit, peer, identity and host-path lines are admin-only; script/component logs stay public',
    'PollEvents (and other log-returning surfaces) filter by the caller role; no-token loopback keeps full visibility; no sequence leak of hidden lines',
    'Per-role tests and a golden list of admin-only prefixes; no protocol change',
    'All 10 checks green; docs verified against code',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: every audit/rate-limit/peer/identity/path log site is tagged (grep all Logger calls in editor-server), nothing sensitive reaches editor/viewer through ANY other surface (health, events, errors), default for untagged lines is safe, sequence handling leaks nothing, no-token loopback behaviour documented and tested, docs true, mediums count.',
}

const a11 = {
  name: 'sec-audit11-sanitised-errors-adminonly-luaseed',
  repo: 'engine',
  branch: 'feat-sec-audit11-sanitised-errors',
  title: 'Security audit item 11 (F10), MCP0b: sanitised errors (no host paths or raw exception text to clients; res://-relative paths) and determinism.luaSeed in SetProjectSettings made admin-only',
  closes: null,
  issue: `Security audit item 11 / finding F10 of ${AUD} (read F10 and the table row 11) and ${MCP} (MCP0). The user accepted ALL recommendations on 2026-10-20. TWO PARTS. PART 1 (F10): error and success responses carry absolute host paths and raw exception text (ReadTextAsset, WritePlaySnapshot, SaveSceneToFile, asset commands, import errors, scene file errors, ...): map engine/filesystem exceptions to GENERIC messages for REMOTE (non-local) clients and keep the detail in the server log (admin-only after item 10: a sibling item 'sec-audit10-viewer-event-filtering' tags log lines; if its tagging API is not merged yet, log the detail with the existing logger and do not depend on it); return snapshot and asset paths as res://-relative (or .n2/play/<name>) when the client is not local; a LOCAL client (loopback/no tokens, the single local user) may keep detail: define 'local' precisely (reuse the host's existing loopback/identity notion) and test both. Find every response string that includes a path or e.what() (grep editor-server/src for e.what(), std::filesystem path to_string/generic_string in response builders, 'Failed to' messages) and sanitise each with a helper (one function, one place), keeping messages actionable (what failed, which res:// path, a stable reason like 'file not found', 'permission denied', 'invalid format'). PART 2: SetProjectSettings is a Write-class command, so an editor-role user can set determinism.luaSeed, which the engine itself flags as a security trade-off (see docs/scripting-lua.html determinism section and the SetProjectSettings handler): make a change to the determinism block (at minimum determinism.luaSeed; check whether other keys in that block or network/physics keys have the same class of risk and report, but only change luaSeed unless the code shows a stronger reason) REQUIRE the admin role, answering the existing forbidden error code for a non-admin with a clear message; the editor UI will adapt later (frontend item); document; add tests (editor role refused with forbidden and nothing written; admin allowed; viewer refused as today; a no-token loopback local user keeps today's behaviour: state and test). TESTS: golden-message tests for ReadTextAsset and WritePlaySnapshot failures (remote vs local), a table test that scans response builders for forbidden substrings (an absolute path prefix such as the project root or drive letter) using a temp project under an unusual absolute path, the luaSeed role tests. No protocol change (allowProtocol false: STOP and report if needed). DOCS: docs/logging-and-editor.html (error text and paths per client kind), docs/editor.html (roles: luaSeed admin-only), docs/scripting-lua.html determinism section, docs/testing.html. Verify every docs sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds.`,
  allowedPaths: ['^editor-server/', '^engine/', '^tests/', '^docs/', '^CMakeLists\\.txt$'],
  acceptance: [
    'No absolute host paths or raw exception text reach remote clients in any response; res://-relative paths; detail kept in the server log; local client behaviour defined and tested',
    'determinism.luaSeed in SetProjectSettings requires admin (forbidden for editors), tested; other risky keys reported',
    'Golden-message tests and a response-path scan test; no protocol change',
    'All 10 checks green; docs verified against code',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: grep every response builder for paths and e.what() (nothing missed), one sanitising helper, the local-vs-remote definition is sound (a remote client cannot be classified local), luaSeed gate cannot be bypassed through another command (SetComponentFields, WriteTextAsset to project.n2proj, import settings?): check that project settings cannot be written some other way, docs true, mediums count.',
}

const f1 = {
  name: 'fe-f1-panel-registry',
  repo: 'frontend',
  branch: 'feat-fe-f1-panel-registry',
  title: 'Electron UI F1: panel registry with tab stacks in the existing regions, Window menu, and per-project persisted layout with reset (foundation for the roadmap features, E-X4 and the Network panel)',
  closes: null,
  issue: `Electron UI roadmap ${ROAD}: the user accepted ALL recommendations U1-U21 on 2026-10-20. Read sections 0-3 (current state, F1, F2) and the decisions U1, U2, U21 FIRST, then the frontend README (Architecture, panels) and the renderer code it names: src/renderer/editor.tsx, panels.tsx, ui.tsx, store.ts, the CSS, the existing splitter handling, and every panel mount (hierarchy-panel.tsx, inspector-panel.tsx, assets-panel.tsx, console-panel.tsx, viewport-panel.tsx, welcome.tsx). THIS ITEM IS F1 ONLY (F2: prefs store + keymap is the NEXT item: do not build the keymap; but build the small persistence layer F1 needs behind an interface F2 can adopt). SCOPE per U1/U2: (1) a lightweight PANEL REGISTRY (src/renderer/panel-registry.ts or similar, framework-free, unit-testable): panels register {id, title, icon?, component, defaultRegion, closable, singleton, minSize?}; four existing regions (keep the current layout as the DEFAULT arrangement, pixel-comparable in structure); each region holds a TAB STACK (one or more panels, active tab); panels can be moved between regions (drag a tab to another region's tab bar, plus a 'Move to' menu for keyboard users), closed (if closable) and reopened from a new Window menu, which also has 'Reset layout'; NO docking library and NO floating windows (U1); (2) a layout model: serialisable {version, regions: {id: {tabs: [panelId], active, size}}, splitter sizes} with validation and migration (unknown panel ids dropped, missing panels appended to their default region, corrupt or newer-version data falls back to the default without throwing); (3) PERSISTENCE per project (U2): renderer localStorage keyed by the project's projectId (find how the app identifies a project: projectId from GetProjectInfo or the host launcher's recent-projects store) with try/catch around every storage access (storage may be unavailable), a user-level default layout key, debounced saves, per-machine only; Window > Reset layout clears the project's layout; (4) migrate every existing panel to register through the registry with NO behaviour change to the panels themselves; the splitter sizes now persist (today they are session-only); the viewport panel keeps its canvas lifecycle when tabs switch (hidden tab: keep mounted or pause rendering: decide per the roadmap section and record it: the viewport must not leak frame requests while hidden); (5) accessibility: tab bar roles (tablist/tab/tabpanel), keyboard navigation (arrows, Home/End), focus visible; (6) TESTS: node unit tests for the layout model (validate/migrate/corrupt/newer version, move/close/reopen/reset reducers), DOM tests (src/test-dom) for tab switching, move between regions, close/reopen via Window menu, reset, restore from storage, storage-unavailable path, viewport frame requests stop while hidden; keep ALL existing tests green; (7) README: the layout/Window menu section (match the README's style and verify against the code). Frontend CI runs typecheck, build, node and DOM tests: you CAN run them locally (npm run typecheck; npm test): do, before pushing. Run the app-level checks you can; list what needs the user (the feel of tab drag, real Electron window) as unverified. Frontend rules: requiresDocs false (README only), no engine/protocol change, no token in logs/storage, never persist secrets in the layout.`,
  allowedPaths: ['^package\\.json$', '^package-lock\\.json$', '^README\\.md$', '^src/', '^scripts/'],
  acceptance: [
    'Panel registry with tab stacks in the four regions, Window menu (open/close/reset) and keyboard-accessible tab bars; default arrangement unchanged',
    'Layout persisted per project (localStorage, try/catch) with validation/migration and a user default; splitter sizes persist; reset works',
    'All existing panels registered with no behaviour change; viewport does not leak frame requests while hidden',
    'Unit and DOM tests; typecheck, build, tests and frontend CI green; README updated',
  ],
  requiresDocs: false,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: no behaviour change to existing panels (diff mounts), layout validation never throws on corrupt storage, storage access always guarded, timers/listeners cleaned up on unmount, viewport hidden-tab handling leaks nothing, keyboard accessibility of the tab bar, no secret ever stored, tests meaningful (fail on regressions), README true, mediums count. Unverified: drag feel in a real Electron window.',
}
for (const it of [a10, a11, f1]) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log('ok')
