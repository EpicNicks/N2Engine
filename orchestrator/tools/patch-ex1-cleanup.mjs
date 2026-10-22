import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const rd = (n) => JSON.parse(fs.readFileSync(D + n + '.json', 'utf8'))
const wr = (n, o) => fs.writeFileSync(D + n + '.json', JSON.stringify(o, null, 2) + '\n')

const ex1 = rd('lua-ex1-sandbox-profiles')
const before = ex1.issue
ex1.issue = ex1.issue.replace(
  /and the PURE-LUA MATH PRELUDE \(the design explains why[^)]*\)/,
  'and a PRELUDE MECHANISM with an EMPTY prelude (USER DECISION 2026-10-20: the design names no prelude contents beyond pure-Lua Vector3/Quaternion helpers in the 4.2 table, which are designed in E-X3 when Editor.Scene values need them, reusing the L1 evaluator stubs; so E-X1 ships only the hook that runs a prelude chunk in a fresh state under the load budget, with a test that the hook runs a given chunk and that an erroring or looping prelude is reported as data; there is NO math.random/determinism rationale in the design, do not invent one)'
)
ex1.issue = ex1.issue.replace('the pure-Lua math prelude', 'the empty-by-default prelude hook')
if (ex1.issue === before) throw new Error('ex1 issue text not patched')
ex1.acceptance = ex1.acceptance.map((a) => a.replace('pure-Lua math prelude', 'prelude hook (empty in E-X1)'))
ex1.notes = ex1.notes.replace('allowlist test is meaningful', 'prelude hook is empty and safe, allowlist test is meaningful')
wr('lua-ex1-sandbox-profiles', ex1)

const c = rd('cleanup-hostcpp-comments-application-docs')
if (!c.allowedPaths.includes('^docs/logging-and-editor\\.html$')) c.allowedPaths.push('^docs/logging-and-editor\\.html$')
c.issue += ' USER-APPROVED WIDENING (2026-10-20): also fix docs/logging-and-editor.html (the --project row around line 219 says Init reads projectPath only for physics.gpu: it reads four settings, verify against Application::Init and match docs/application.html) and the user-visible --help strings in editor-server/src/HostOptions.cpp (--no-rate-limit, --preauth-slots ~517-534) that say everyone behind a proxy is loopback: correct them to what the code does (loopback only when the proxy connects over loopback), text only, and CHECK that no test or doc compares the --help output (grep tests/ and docs/ for those strings; update any that quote them).'
wr('cleanup-hostcpp-comments-application-docs', c)
console.log('ok')
