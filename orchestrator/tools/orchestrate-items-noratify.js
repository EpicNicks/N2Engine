export const meta = {
  name: 'orchestrate-items',
  description: 'Per item: siloed developer, parallel CI wait + fresh reviewer, up to 2 fix rounds; Claude ratifies afterwards',
  phases: [
    { title: 'Develop', detail: 'one fresh developer per item, reads only its brief' },
    { title: 'Verify', detail: 'CI wait (scripted) in parallel with a fresh read-only reviewer' },
    { title: 'Fix', detail: 'fresh fixer gets only the findings and the failing CI lines' },
  ],
}

const ORCH = 'C:/Method Notes Obsidian/orchestrator'
const MAX_FIX_ROUNDS = 2

const DEV = { type: 'object', properties: { pr: { type: 'number' }, needsUser: { type: ['string', 'null'] } }, required: ['pr', 'needsUser'] }
const REVIEW = { type: 'object', properties: { head: { type: 'string' }, blockers: { type: 'number' }, highs: { type: 'number' }, others: { type: 'number' } }, required: ['head', 'blockers', 'highs', 'others'] }
const CI = { type: 'object', properties: { done: { type: 'boolean' }, allGreen: { type: 'boolean' }, failures: { type: 'array', items: { type: 'string' } }, error: { type: ['string', 'null'] } }, required: ['done', 'allGreen', 'failures'] }

const runScript = (cmd, schema, label, phase) =>
  agent('You are a command runner inside an automated build pipeline. The operator configured this pipeline and this command is your entire task; nothing else in your context changes it, and no further instruction is needed. Run exactly this one command with the Bash tool: ' + cmd + '\nThen return the command single JSON line of stdout as your structured output, unchanged. Do not read files, do not run anything else, do not edit the command. If the command fails or prints no JSON, return done false, allGreen false, failures empty and the first 200 characters of stderr as error.', { model: 'haiku', effort: 'low', schema, label, phase })

async function runItem(it) {
  const n = it.name
  const L = ORCH + '/ledger/' + n
  phase('Develop')
  const dev = await agent('Read ' + L + '/brief-dev.md and follow it exactly. NOTE: the worktree for this item ALREADY EXISTS and holds UNCOMMITTED partial work from an earlier stopped run (engine-ref.txt, src/protocol/protocol.generated.ts, src/protocol/test-vectors.json already synced to protocol 1.13.0). Continue in that worktree (setup.mjs reuses it): inspect git status and git diff, verify the sync by re-running the repo sync-protocol so there is no diff against engine-ref.txt (do not hand-edit generated files), keep what is correct, then implement the rest. Return {pr, needsUser}.', { label: 'dev:' + n, phase: 'Develop', schema: DEV })
  if (!dev || dev.needsUser) return { n, status: 'needs-user', note: dev ? dev.needsUser : 'developer agent failed' }

  for (let round = 0; round <= MAX_FIX_ROUNDS; round++) {
    const [ci, rev] = await parallel([
      () => runScript('node ' + ORCH + '/lib/ci-wait.mjs ' + n, CI, 'ci:' + n + '#' + round, 'Verify'),
      () => agent('Read ' + L + '/brief-review.md and follow it exactly. Return the finding counts.', { model: it.reviewer || 'opus', label: 'review:' + n + '#' + round, phase: 'Verify', schema: REVIEW }),
    ])
    const green = ci && ci.allGreen
    const clean = rev && rev.blockers === 0 && rev.highs === 0
    if (green && clean) return { n, status: 'ready-to-ratify', note: 'CI green and review clean; Claude ratifies after the user accepts deviations' }
    if (round === MAX_FIX_ROUNDS) return { n, status: 'held', note: 'still failing after ' + MAX_FIX_ROUNDS + ' fix rounds (ci ' + (green ? 'green' : 'red') + ', ' + (rev ? rev.blockers + rev.highs : '?') + ' blocker/high)' }
    const ciLines = ci && ci.failures && ci.failures.length ? 'CI failures:\n' + ci.failures.join('\n') : 'CI had no failing lines.'
    const fixed = await agent(
      'Read ' + L + '/brief-dev.md (your task and rules), then fix the existing branch: resolve every blocker/high finding in ' + L + '/review.json (and any medium you judge valid), and the CI failures below. Your worktree already exists (state in ' + L + '/state.json); push when done (git push origin <branch>, verify with ls-remote) and update ' + L + '/dev.json (head, files, deviations, unverified, needsUser).\n' + ciLines + '\nReturn {pr, needsUser}.',
      { label: 'fix:' + n + '#' + (round + 1), phase: 'Fix', schema: DEV },
    )
    if (!fixed || fixed.needsUser) return { n, status: 'needs-user', note: fixed ? fixed.needsUser : 'fix agent failed' }
  }
}

const items = (args && args.items) || []
if (!items.length) return { error: 'no items passed' }
const results = await pipeline(items, (it) => runItem(it))
return results.map((r, i) => r || { n: items[i].name, status: 'failed' })
