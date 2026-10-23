import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
for (const n of ['fe-fe3-rotate-scale-gizmos', 'fe-fe6-fe7-filters', 'fe-fe8-history-panel', 'fe-fe5-inspector-local-transform']) {
  const f = D + n + '.json'
  const it = JSON.parse(fs.readFileSync(f, 'utf8'))
  it.requiresDocs = false
  fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
  console.log(n, it.requiresDocs)
}
