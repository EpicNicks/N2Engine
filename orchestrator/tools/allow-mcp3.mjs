import fs from 'node:fs'
const f = 'C:/Method Notes Obsidian/orchestrator/items/mcp3-play-tools.json'
const o = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!o.allowedPaths.includes('^mcp/')) o.allowedPaths.push('^mcp/')
fs.writeFileSync(f, JSON.stringify(o, null, 2) + '\n')
console.log(o.allowedPaths.join(' '))
