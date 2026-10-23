import fs from 'node:fs'
const f = 'C:/Method Notes Obsidian/orchestrator/items/sec-preauth-s5-docs.json'
const it = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!it.allowedPaths.includes('^deploy/')) it.allowedPaths.push('^deploy/')
fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
console.log(it.allowedPaths)
