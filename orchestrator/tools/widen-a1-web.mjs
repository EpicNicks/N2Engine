import fs from 'node:fs'
const f = 'C:/Method Notes Obsidian/orchestrator/items/net-a1-presets-settings.json'
const it = JSON.parse(fs.readFileSync(f, 'utf8'))
if (!it.allowedPaths.includes('^web/player/WebPlayer\\.cpp$')) it.allowedPaths.push('^web/player/WebPlayer\\.cpp$')
it.issue += ' ORCHESTRATOR WIDENING (2026-10-20, after the re-review at cc3fe97): (a) merge origin/master into the branch (A2 #186, E-X2 #189, G0 #188 are merged; resolve Application.cpp carefully); (b) wire network.preset to the simulation mode that A2 added: at Init, preset authoritative or deterministic must call Application::SetSimulationMode(Authoritative|Deterministic) and none must leave None (A2 left this to A1), with tests (the none path byte-for-byte unchanged); (c) correct the now-stale text that says the timeScale-independent tick loop is not built yet (NetworkValidation.cpp ~117 requirement text, NetworkPresets.cpp ~92 tick-driver row, docs/physics.html ~173 and ~250, docs/application.html ~486): say what is true after A2 + this wiring; (d) allowed: web/player/WebPlayer.cpp ONLY to show the engine refusal reason instead of the empty "could not start (no renderer)" message on a refused project.'
fs.writeFileSync(f, JSON.stringify(it, null, 2) + '\n')
console.log('ok', it.allowedPaths)
