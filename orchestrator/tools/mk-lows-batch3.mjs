import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const src = fs.readFileSync('C:/Method Notes Obsidian/orchestrator/tools/mk-batch-1022.mjs', 'utf8')
const COMMON = eval(src.match(/const COMMON = (' .*?')\n/s)[1])
const FECOMMON = ' Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo is READ-ONLY reference. npm run typecheck and npm test must pass locally before you push; CI has 3 checks. NEVER run taskkill; no attribution lines on commits or PRs; GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; merge origin/master into the branch if it moved before your final push; your FINAL message must be a real report (PR number, head sha, what you did, deviations).'
const items = []
const add = (o) => items.push({ closes: null, allowProtocol: false, gate: null, approved: true, requiresDocs: true, repo: 'engine', ...o })

add({
  name: 'phys-lows-j4-j5',
  branch: 'fix-phys-lows-j4-j5',
  title: 'Physics follow-ups: trigger Exit after a speculative-only contact, Persisted/re-Enter docs, RaycastAll tie order, PhysX single-hit query cost, no-backend log level, design-jolt-queries index row',
  issue: `Follow-ups of merged #211, J4 (#213), J5 (#212); the user accepted these LOW review findings as a follow-up. Verify each against the CODE on master first (git grep to locate), fix only what is real, and say in the report which you did not change and why. (1) J4: in the Jolt contact folding (JoltBackend.cpp FoldContacts / the presentPairs set) presentPairs is filled for every present report including a trigger contact that is only inside the speculative distance (touching false); the old key of a tracked trigger touch is then erased silently while the new key's info stays reported=false, so the tracker keeps the trigger touch and no later parted/ended path can send its Exit: fix so a trigger touch is never left without an Exit path (only count a pair present when it is actually touching, or carry the reported flag over to the new key) with a test (a trigger starting speculative-only then touching then leaving: exactly one Enter and one Exit). (2) J4 docs/physics.html: the sentence 'every Added, the Persisted of trigger contacts, every Removed' no longer describes the listener (solid Persisted reports of untracked contacts are buffered too) and nothing says that a touch begun again after SetIsTrigger or a collision-matrix change starts with an Enter that carries no contact points, impulse or relativeVelocity: make the docs true against the code. (3) J5: RaycastAll orders by exact float distance with no kQueryTieTolerance, unlike the single-hit pick (QueryUtil): near-equal hits are ordered by float noise: apply the same tolerance + handle tie-break ordering as the single-hit pick on both backends with a test. (4) J5: every PhysX single-hit query (Raycast::Single/Any, SphereCast, CapsuleCast) now runs a second touching scene query and value-initialises a 1024-entry hit buffer on the heap: avoid the per-query heap allocation of the 1024-entry buffer (reuse a member scratch buffer or stack/small buffer) and the second query where it is not needed, WITHOUT changing results; keep the existing tests passing. (5) #211: a build with no backend at all (web leg PHYSX=OFF and JOLT off) logs an ERROR on every start because the hosts' PHYSX request is 'refused'; before it was an Info 'Built without PhysX': when the build has NO backend compiled in, log at Info/Warn level with a clear message instead of an Error (keep Error for a real refusal such as Deterministic needing features); update the HealthTests/docs strings accordingly. (6) docs/index.html design-decisions table has no row for physics.html#design-jolt-queries (a callout for the user to accept whose decisions 5 and 6 were rewritten at the merge): add the row in the table's existing style, true against physics.html. Nothing builds locally; re-read every edit twice and trace identifiers.${COMMON}`,
  allowedPaths: ['^engine/', '^tests/', '^docs/'],
  acceptance: ['Trigger touch always has an Exit path (test)', 'RaycastAll tie ordering matches single-hit on both backends (test)', 'No per-query 1024-entry heap allocation or redundant second query; results unchanged', 'No-backend build logs below Error; docs and index row true; CI green'],
  reviewer: 'opus',
  notes: 'Reviewer (concurrency/numerics): the speculative-contact fix cannot produce a double Enter/Exit or lose an Exit, tie tolerance semantics identical on PhysX and Jolt, the scratch buffer is not shared across threads or reentrant calls, no behaviour change in query results, compile-level correctness of every identifier.',
})

add({
  name: 'web-g4-e6-lows',
  branch: 'fix-web-g4-e6-lows',
  title: 'Follow-ups: G4 async-readback coverage, WebGpuFactory comment, isHeadless docs row, web-build size table; E-X6 code lows',
  issue: `Follow-ups of merged G4 (#208) and E-X6 (#215); the user accepted these LOW findings as a follow-up. Verify each against the CODE on master first and say what you did not change and why. G4/web: (1) the async readback path of the WebGPU renderer compiles but no test runs it (engine/src/renderer/.../WebGpuRenderer.cpp ~line 428): add the best coverage achievable without a browser GPU (a test in the web node tests or a pure-logic test of the readback state machine/ordering, e.g. factor the pure part out) or, if truly impossible, a clearly marked TODO in docs 'Not verified' naming G5/G6; (2) engine/include/.../WebGpuFactory.hpp comment says the factory is declared only where compiled in but the header declares it unconditionally: make comment and code agree (prefer fixing the comment unless the conditional declaration is trivially right); (3) docs/application.html isHeadless row (~line 128) and the Headless mode intro name only SOFTWARE and WEBGPU as the backends without a window: NONE also has no window: say so; (4) docs/web-build.html #size-report: the Limit column still shows the pre-Jolt limits (a J3 leftover) while web/budgets.json holds the raised ones: make the table match web/budgets.json exactly (read the file; do not invent numbers) and check the sentences around it. E-X6: (5) OnBeforePlay runs and broadcasts its warnings before WritePlaySnapshot validates the request, with the unvalidated client scene path in the message: move the hook after validation or sanitise the path in the message; test; (6) an extension's own asset write never reaches any OnAssetsChanged hook because NoteAssetWritten refreshes the index so no later poll sees a change: this was a DECLARED deviation, so only make the docs state it plainly and precisely if they do not already; (7) the watcher never retries a file that failed on an id clash when the owning file changes, and the untouched extension always wins: retry the failed file when the clash owner changes or is removed; test. Also one tiny frontend-unrelated engine item: none.${COMMON}`,
  allowedPaths: ['^engine/', '^editor-server/', '^web/', '^tests/', '^docs/', '^\\.github/'],
  acceptance: ['Readback covered or honestly documented; WebGpuFactory comment true', 'Docs rows (isHeadless, size table) true against code and budgets.json', 'OnBeforePlay hook runs after validation; watcher retries an id-clash file; CI green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: size table equals web/budgets.json exactly, hook ordering test, retry cannot loop or reload an unchanged file repeatedly, docs true.',
})

add({
  name: 'fe-ex5-test-title',
  repo: 'frontend',
  requiresDocs: false,
  branch: 'fix-ex5-test-title',
  title: 'Frontend: test title still said protocol 1.15.0 after the 1.16.0 sync',
  issue: 'Follow-up of merged frontend E-X5 (#54): src/test/engine-client-extensions.test.ts (~line 138) has a test title that says "the client is at 1.15.0" while its assertion was bumped to 1.16.0: fix the title (and grep src/ and mcp/ tests for any other leftover "1.15.0" in titles or comments describing the current protocol version; leave historical statements about 1.15.0 features alone). Tiny item: one commit.' + FECOMMON,
  allowedPaths: ['^src/', '^mcp/', '^README\\.md$'],
  acceptance: ['No test title or comment names the wrong current protocol version', 'typecheck, tests and 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: only titles/comments changed, no assertion changed.',
})

for (const it of items) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log(items.map(i => i.name).join('\n'))
