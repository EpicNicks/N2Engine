import fs from 'node:fs'
// usage: node tools/bump-review-head.mjs <item> <newHead> <note>
const [item, head, note] = process.argv.slice(2)
const f = `C:/Method Notes Obsidian/orchestrator/ledger/${item}/review.json`
const r = JSON.parse(fs.readFileSync(f, 'utf8'))
r.head = head
r.confirmedDelta = note
fs.writeFileSync(f, JSON.stringify(r, null, 2) + '\n')
console.log('review head', head)
