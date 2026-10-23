import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const P = 'C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md'

const ex1 = {
  name: 'lua-ex1-sandbox-profiles',
  repo: 'engine',
  branch: 'feat-lua-ex1-sandbox-profiles',
  title: 'Lua editor extensions, PR E-X1: shared LuaSandbox builder with Declarations and EditorExtension profiles (libraries, no load, trimmed string, budgets, pure-Lua math prelude), not wired to any host feature',
  closes: null,
  issue: `Design APPROVED by the user (D1-D12, 2026-10-15): ${P}. Read sections 0, 1.2, 2, 4, 6.4 ('Sandbox and budgets'), 6.5 ('How this fits option B'), 9 (staged plan: this is PR E-X1) and 10 FULLY first. This item is ONLY E-X1: a library component, NOT wired into the editor host, no protocol change, no flag. MERGED prerequisites to read and REUSE (do not duplicate): L1 #175 (engine/include/engine/scripting/DeclarationEvaluator.hpp, engine/src/scripting/DeclarationEvaluator.cpp: it already builds a sandboxed throwaway state with budgets and an inert-stub environment: find how it sets up libraries/budgets/allocator and factor the COMMON part out), L2 #179 (ScriptDeclarationCache with an off-thread worker and deadline, execution policy), #159 (budgets, sandbox prelude, LuaBudgetScope, panic repair, RunChunk) and #173 (registry reserve). Read docs/scripting-lua.html (sandbox, evaluator, L2 sections) too. SCOPE: (1) a shared builder, e.g. Scripting::LuaSandbox::Create(Profile) -> an owning handle to a fresh lua_State with its own allocator cap and instruction/wall-clock hook (reuse the existing mechanisms; do not touch the global game state or LuaRuntime::GetState()): profile Declarations = exactly what L1's evaluator uses today (REFACTOR the evaluator to use the builder with ZERO behaviour change: its existing tests are the oracle and must stay green unchanged); profile EditorExtension = the libraries per design 6.4 (base minus load/loadstring/dofile/loadfile/collectgarbage-as-needed/rawset-escape hatches as the design says, table, math, utf8 as the design says, a TRIMMED string library WITHOUT pattern matching (D6: omitted in v1; string.rep etc. still capped by the allocator), no io/os/debug/package/require), the default budgets of D7 as named constants (load 10M instructions / 1 s, invocation 200M / 5 s, hook 20M / 0.5 s, 64 MiB per state; the nested-command cap 10,000 is a constant for later PRs, just declare it here) with per-call overrides for tests, and the PURE-LUA MATH PRELUDE (the design explains why: math.random/determinism and anything not safe is replaced by pure-Lua definitions; read 6.4 and implement exactly what it lists, nothing more). (2) NO engine bindings and no editor API in either profile (the Editor namespace is E-X3): a 'globals of the state' test lists the global names of each profile and compares to an allowlist so any later addition is a conscious change. (3) Every path closes the state (RAII) and reports script errors, budget exhaustion (instructions AND wall clock) and memory exhaustion as DATA, never crashing; the known weak-key GC C-call stall (L1/L2 finding) is NOT solved here (the caller runs on a worker with an abandon deadline, as ScriptDeclarationCache does): document it in the builder's header and docs as a precondition; do not patch vendored Lua. (4) TESTS (engine unit, all legs; web too if the Lua tests run there: the web build has no threads so keep the code thread-free): while true do end and a pcall-wrapped loop => budget trip in bounded time; string.rep('x', 1e9) and a table growth bomb => memory exhausted not crash; io/os/load/dofile/require/debug/package absent in EditorExtension; string.find/match/gmatch/gsub absent in EditorExtension (D6) while string.format/rep/sub/len/etc. behave; the math prelude values/behaviour; the globals-allowlist tests for both profiles; two states alive at once are independent; the Declarations profile through the builder gives identical results to before (existing DeclarationEvaluatorTests unchanged and green); nothing leaks (allocator counters). (5) DOCS: a short section in docs/scripting-lua.html describing the builder, the two profiles and their limits (verify every sentence against the code), docs/testing.html test list; do NOT write docs/editor-extensions.html yet (E-X7). Nothing builds locally; CI (10 checks) is the test; expect fix rounds. Known limitations to respect: do not rely on Lua error TEXT read off the stack (MSVC Debug/wasm); sol reference creation is noexcept and terminates on luaL_ref failure at the memory cap (reserve-then-copy, #173); MSVC Debug and wasm errors are C++ exceptions/longjmp through C++ frames. Unverified to list: wasm/MSVC-Debug specifics, anything needing the editor host.`,
  allowedPaths: ['^engine/', '^tests/', '^docs/scripting-lua\\.html$', '^docs/testing\\.html$', '^CMakeLists\\.txt$'],
  acceptance: [
    'A shared LuaSandbox builder with Declarations and EditorExtension profiles exists, not wired to any host feature',
    'DeclarationEvaluator uses the builder with zero behaviour change (existing tests green unchanged)',
    'EditorExtension profile: trimmed libraries, no string patterns, no io/os/load/debug/package/require, D7 default budgets, pure-Lua math prelude, globals allowlist tests',
    'Budget, memory and error outcomes reported as data; state closed on every path; tests per design 9',
    'All 10 checks green; docs verified against code',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'opus',
  notes: 'Reviewer: sandbox completeness for BOTH profiles (no escape through base library functions, metatables of strings, rawset/getmetatable tricks, coroutine, loadstring aliases, string.dump, os/io reachable through any upvalue), budgets really apply to the new state and not the game state, the Declarations refactor changed no behaviour (diff the evaluator semantics), allowlist test is meaningful, nothing leaks, docs true. Unverified: wasm/MSVC-Debug specifics.',
}

const cleanup = {
  name: 'cleanup-hostcpp-comments-application-docs',
  repo: 'engine',
  branch: 'fix-hostcpp-comments-app-docs',
  title: 'Tidy: stale comments in editor-server/src/Host.cpp (one-at-a-time server, everything behind a proxy is loopback) and the docs/application.html Init sentence that predates the three settings Init reads',
  closes: null,
  issue: `Small cleanup, comments and docs only, no behaviour change. (1) editor-server/src/Host.cpp around lines 486-492: the comment 'The deadlines that keep one connection from holding the one-at-a-time server' is stale since PR #176 (acceptor + pre-auth workers + a session thread; a pre-Hello connection holds one pre-auth slot, the deadlines bound how long) and 'Everything behind a proxy is loopback, which has no per-address limits (see the docs)' is wrong in general: the host sees the peer address the proxy/container presents, which is loopback only when the proxy connects over loopback (docs/editor.html and docs/logging-and-editor.html, 'behind a proxy' text, now say the right thing since PR #180: read them and match them, verify against EditorServer.cpp/RateLimiter.cpp/PreAuthSlots.hpp/Host.cpp). Search editor-server/ and engine/ for any OTHER comment with the same stale claims (grep 'one-at-a-time', 'one at a time', 'serial', 'serving thread', 'loopback backoff', 'shared backoff') and correct only comments that are verifiably stale; do not touch code. (2) docs/application.html around line 119 (the Config::ApplicationOptions table, row projectPath and the surrounding prose): 'Init does not report a bad value of either' (or similar) predates Init reading THREE project settings (determinism.luaSeed, math.simdTier and the third one: read Application::Init in engine/src/Application.cpp to see exactly which and what Init does with a bad value of each: report/ignore/default); rewrite the sentence so it is TRUE for all three, verified against the code, and check the 'four fields' wording of the table against the struct in the header. Re-read every edit (comments must still compile: balanced comment markers). Native behaviour must be byte-identical (comment-only C++ edits). No other changes.`,
  allowedPaths: ['^editor-server/src/', '^editor-server/include/', '^engine/src/', '^engine/include/', '^docs/application\\.html$'],
  acceptance: [
    'Host.cpp comments (and any sibling stale comments found) match the post-#176/#177 code',
    'docs/application.html Init sentence true for all three project settings, verified against Application::Init',
    'Comment-only C++ edits; all checks green',
  ],
  requiresDocs: true,
  allowProtocol: false,
  gate: null,
  approved: true,
  reviewer: 'sonnet',
  notes: 'Reviewer: C++ diff is comments only (strip comments and compare, or inspect each hunk), comment syntax balanced, every changed sentence true against the code, docs sentence true for all three settings.',
}
for (const it of [ex1, cleanup]) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log('ok')
