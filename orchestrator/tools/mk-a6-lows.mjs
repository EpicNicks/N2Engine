import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const src = fs.readFileSync('C:/Method Notes Obsidian/orchestrator/tools/mk-batch-1022.mjs', 'utf8')
const COMMON = eval(src.match(/const COMMON = (' .*?')\n/s)[1])
const it = {
  closes: null, allowProtocol: false, gate: null, approved: true, requiresDocs: true, repo: 'engine',
  name: 'net-a6-lows',
  branch: 'fix-net-a6-lows',
  title: 'N2Server follow-ups: orderly shutdown on the exception path, accurate stdout/Debug and token-check docs, a PeerConnected-without-identity session really dropped',
  issue: `Follow-up of merged A6 (#216, the N2Server executable); the opus review found four LOW issues that the user accepted as a follow-up. Verify each on master first (find the files with git grep N2Server / n2server). (1) The std::exception catch in N2Server's main returns 1 without OnApplicationQuit or app.Shutdown(), so scenes, physics and audio are torn down only by static destruction, which can crash or hang: run the same orderly shutdown path as the normal exit (guarded, never throwing), keep exit code 1; add a test if the structure allows (an injectable failure) else explain. (2) The docs claim 'nothing else is written to stdout' but in Debug builds Application::Init calls Logger::InitializeDebugConsoleHelper (N2ENGINE_DEBUG) which echoes log events to the console: either make the server suppress that helper so the claim is true (preferred: the ready line must stay the only stdout line a supervisor needs to parse, check how the editor host handles it) or correct the sentence precisely; test or docs verified against the code. (3) The docs Security section says join proofs are matched with BasicTokenTable::Authenticate but SecureTransport::OnConnectResponse runs its own constant-time HMAC loop: reword to what the code does (still constant time, still the same tokens file semantics) after reading both. (4) When SecureTransport reports PeerConnected without an identity, the server logs that the session 'is dropped' but nothing disconnects it (SecureTransport::Disconnect is a no-op inside Poll's handler): make the server really drop it (e.g. queue the disconnect and apply after Poll returns, mirroring SecureTransport's deferredInnerDisconnects_ pattern) and add a test, or if the situation cannot occur in practice change the log text to say so and justify it in the report with the code path. Security-relevant: keep every existing fail-closed rule; no secret in logs.${COMMON}`,
  allowedPaths: ['^engine/', '^net/', '^tests/', '^docs/', '^CMakeLists\\.txt$', '^cmake/', '^\\.github/workflows/', '^tools/', '^n2server/', '^apps/', '^server/'],
  acceptance: ['Exception path shuts down in order with exit code 1', 'stdout/Debug claim true; token-check docs true against the code', 'A PeerConnected without identity is really disconnected (test) or the text justified', 'CI green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: shutdown path cannot throw or deadlock, no secret printed, the drop really happens and cannot be skipped, no change to the bind/token rules, docs true.',
}
fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log(it.name)
