import fs from 'node:fs'
const f = 'C:/Method Notes Obsidian/orchestrator/items/jolt-j3-backend-core.json'
const it = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!it.allowedPaths.includes('^web/budgets\\.json$')) it.allowedPaths.push('^web/budgets\\.json$')
fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
console.log(it.allowedPaths)
