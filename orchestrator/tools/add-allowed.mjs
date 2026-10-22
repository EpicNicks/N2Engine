import fs from 'node:fs'
// usage: node tools/add-allowed.mjs <item> <regex>
const [item, re] = process.argv.slice(2)
const f = `C:/Method Notes Obsidian/orchestrator/items/${item}.json`
const it = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!it.allowedPaths.includes(re)) it.allowedPaths.push(re)
fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
console.log(item, JSON.stringify(it.allowedPaths))
