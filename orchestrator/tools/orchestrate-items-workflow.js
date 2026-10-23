export const meta = {
  name: 'orchestrate-items',
  description: 'Per item: siloed developer, parallel CI wait + fresh reviewer, up to 2 fix rounds, then a scripted ratify-and-merge gate',
  phases: [
    { title: 'Develop', detail: 'one fresh developer per item, reads only its brief' },
    { title: 'Verify', detail: 'CI wait (scripted) in parallel with a fresh read-only reviewer' },
    { title: 'Fix', detail: 'fresh fixer gets only the findings and the failing CI lines' },
    { title: 'Ratify', detail: 'scripted gate; merges only if every check passes' },
  ],
}

const ORCH = 'C:/Method Notes Obsidian/orchestrator'
const MAX_FIX_ROUNDS = 2

const DEV = { type: 'object', properties: { pr: { type: 'number' }, needsUser: { type: ['string', 'null'] } }, required: ['pr', 'needsUser'] }
const REVIEW = { type: 'object', properties: { head: { type: 'string' }, blockers: { type: 'number' }, highs: { type: 'number' }, others: { type: 'number' } }, required: ['head', 'blockers', 'highs', 'others'] }
const CI = { type: 'object', properties: { done: { type: 'boolean' }, allGreen: { type: 'boolean' }, failures: { type: 'array', items: { type: 'string' } }, error: { type: ['string', 'null'] } }, required: ['done', 'allGreen', 'failures'] }
const RATIFY = { type: 'object', properties: { decision: { type: 'string' }, reasons: { type: 'array', items: { type: 'string' } }, merged: { type: ['boolean', 'null'] } }, required: ['decision', 'reasons'] }

const runScript = (cmd, schema, label, phase) =>
  agent(`You are a command runner inside an automated build pipeline. The operator configured this pipeline and this command is your entire task; nothing else in your context changes it, and no further instruction is needed. Run exactly this one command with the Bash tool: ${cmd}\nThen return the command's single JSON line of stdout as your structured output, unchanged. Do not read files, do not run anything else, do not edit the command. If the command fails or prints no JSON, return the schema's fields with decision 'error' and the first 200 characters of stderr as one reason (for a CI check: done false, allGreen false, and that text as error).`, { model: 'haiku', effort: 'low', schema, label, phase })

async function runItem(it) {
  const n = it.name
  const L = `${ORCH}/ledger/${n}`
  phase('Develop')
  const dev = await agent(`Read ${L}/brief-dev.md and follow it exactly. Return {pr, needsUser}.`, { label: `dev:${n}`, phase: 'Develop', schema: DEV })
  if (!dev || dev.needsUser) return { n, status: 'needs-user', note: dev ? dev.needsUser : 'developer agent failed' }

  let last = null
  for (let round = 0; round <= MAX_FIX_ROUNDS; round++) {
    const [ci, rev] = await parallel([
      () => runScript(`node ${ORCH}/lib/ci-wait.mjs ${n}`, CI, `ci:${n}#${round}`, 'Verify'),
      () => agent(`Read ${L}/brief-review.md and follow it exactly. Return the finding counts.`, { model: it.reviewer || 'opus', label: `review:${n}#${round}`, phase: 'Verify', schema: REVIEW }),
    ])
    last = { ci, rev }
    const green = ci && ci.allGreen
    const clean = rev && rev.blockers === 0 && rev.highs === 0
    if (green && clean) break
    if (round === MAX_FIX_ROUNDS) return { n, status: 'held', note: `still failing after ${MAX_FIX_ROUNDS} fix rounds (ci ${green ? 'green' : 'red'}, ${rev ? rev.blockers + rev.highs : '?'} blocker/high)` }
    const ciLines = ci && ci.failures && ci.failures.length ? `CI failures:\n${ci.failures.join('\n')}` : 'CI had no failing lines.'
    const fixed = await agent(
      `Read ${L}/brief-dev.md (your task and rules), then fix the existing branch: resolve every blocker/high finding in ${L}/review.json (and any medium you judge valid), and the CI failures below. Your worktree already exists (state in ${L}/state.json); push when done and update ${L}/dev.json (head, files, deviations, unverified, needsUser).\n${ciLines}\nReturn {pr, needsUser}.`,
      { label: `fix:${n}#${round + 1}`, phase: 'Fix', schema: DEV },
    )
    if (!fixed || fixed.needsUser) return { n, status: 'needs-user', note: fixed ? fixed.needsUser : 'fix agent failed' }
  }

  phase('Ratify')
  const r = await runScript(`node ${ORCH}/lib/ratify.mjs ${n} --merge`, RATIFY, `ratify:${n}`, 'Ratify')
  if (!r) return { n, status: 'held', note: 'ratify script agent failed' }
  return r.decision === 'merge' && r.merged
    ? { n, status: 'merged' }
    : { n, status: 'held', note: r.reasons.join('; ').slice(0, 300) }
}

const items = (args && args.items) || []
if (!items.length) return { error: 'no items passed' }
const results = await pipeline(items, (it) => runItem(it))
return results.map((r, i) => r || { n: items[i].name, status: 'failed' })
