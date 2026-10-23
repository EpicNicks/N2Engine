import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const rd = (n) => JSON.parse(fs.readFileSync(D + n + '.json', 'utf8'))
const wr = (n, o) => fs.writeFileSync(D + n + '.json', JSON.stringify(o, null, 2) + '\n')

const a1 = rd('net-a1-presets-settings')
if (!a1.allowedPaths.includes('^editor-server/')) a1.allowedPaths.push('^editor-server/')
a1.issue += ' ORCHESTRATOR WIDENING (2026-10-20): ^editor-server/ is now allowed ONLY for the review high finding: editor-server/src/Host.cpp must stop (return a failure exit code, with the refusal already logged by Init) when app.Init returns a refused health, before entering play mode or the edit loop; add a test if the host tests allow it (tests/editor-server/).'
wr('net-a1-presets-settings', a1)

const g1 = rd('webgpu-g1-wgsl-validation')
g1.approved = true
g1.gate = null
wr('webgpu-g1-wgsl-validation', g1)
console.log('ok', a1.allowedPaths, g1.approved)
