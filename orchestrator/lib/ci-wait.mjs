// node ci-wait.mjs <item> [--timeout <s>] [--once] : wait for CI on the PR head; print a compact JSON verdict.
import { ledgerDir, loadItem, out, readJson, sleep, writeJson } from "./common.mjs"
import { ciOnce, failureLines } from "./ci.mjs"

const name = process.argv[2]
const flag = (f, d) => { const i = process.argv.indexOf(f); return i > 0 ? process.argv[i + 1] : d }
const once = process.argv.includes("--once")
const timeoutMs = Number(flag("--timeout", 1800)) * 1000
const POLL_MS = 85_000

const item = loadItem(name)
const dev = readJson(`${ledgerDir(name)}/dev.json`)
if (!dev?.pr) { out({ name, error: "no dev.json with a pr number" }); process.exit(0) }

let waited = 0
for (;;) {
  let s = null
  try { s = ciOnce(item, dev.pr) } catch (e) {
    if (once || waited >= timeoutMs) { out({ name, error: String(e.message).slice(0, 200) }); process.exit(0) }
  }
  // done = every run for this exact head sha has completed (an engine PR can have two runs per sha)
  if (s && (s.done || (once && s.runs.length === 0))) {
    const failures = s.done && !s.allGreen ? failureLines(item, s.runs) : []
    const v = { name, head: s.head, done: s.done, allGreen: s.allGreen, runs: s.runs.map((r) => `${r.name}:${r.status === "completed" ? r.conclusion : r.status}`), failures }
    writeJson(`${ledgerDir(name)}/ci.json`, v)
    out(v)
    process.exit(0)
  }
  if (once || waited >= timeoutMs) {
    out({ name, head: s?.head, done: false, allGreen: false, runs: (s?.runs ?? []).map((r) => `${r.name}:${r.status}`), failures: ["timed out waiting for CI"] })
    process.exit(0)
  }
  await sleep(POLL_MS)
  waited += POLL_MS
}
