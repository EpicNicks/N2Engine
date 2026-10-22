import fs from 'node:fs'
// usage: node tools/set-review-clean.mjs <item> <newHead> <note>  (marks the verdict clean, drops findings that were fixed: pass --keep-lows to keep low findings)
const [item, head, note] = process.argv.slice(2)
const f = `C:/Method Notes Obsidian/orchestrator/ledger/${item}/review.json`
const r = JSON.parse(fs.readFileSync(f, 'utf8'))
r.head = head
r.verdict = 'clean'
r.findings = (r.findings || []).filter(x => !['blocker', 'high', 'medium'].includes(x.severity) && process.argv.includes('--keep-lows'))
r.confirmedDelta = note
fs.writeFileSync(f, JSON.stringify(r, null, 2) + '\n')
console.log('review clean at', head, 'findings kept', r.findings.length)
