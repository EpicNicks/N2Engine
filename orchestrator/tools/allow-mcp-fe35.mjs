import fs from 'node:fs'
const f = 'C:/Method Notes Obsidian/orchestrator/items/frontend-protocol-1-14.json'
const it = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!it.allowedPaths.includes('^mcp/')) it.allowedPaths.push('^mcp/')
fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
console.log(it.allowedPaths)
