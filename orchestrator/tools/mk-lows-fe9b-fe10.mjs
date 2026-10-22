import fs from 'node:fs'
const FECOMMON = ' Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test must pass before you push; CI has 3 checks. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.'
const it = {
  closes: null, allowProtocol: false, gate: null, approved: true, requiresDocs: false, repo: 'frontend',
  name: 'fe-lows-fe9b-fe10',
  branch: 'fix-fe9b-fe10-lows',
  title: 'Electron UI follow-ups: no-op Apply on an invalid stored layer matrix, import-settings draft/edited fixes',
  issue: 'Follow-up of merged frontend PRs FE9b (#48) and FE10 (#49); the user accepted these LOW review findings as a follow-up. Verify each against the code on master before changing it. (1) FE9b: with an invalid stored collision-layer matrix, a no-op Apply leaves it invalid (src/renderer/project-settings-schema.ts ~line 516): Apply with unchanged values must not leave an invalid matrix in place silently: either normalise/repair it on Apply or keep Apply blocked with a clear message, whichever fits the existing validation flow; add a test. (2) FE10: an invalid number draft keeps Apply blocked after switching to the JSON view (assets-panel.tsx ~line 263): switching view must discard or surface the invalid draft so Apply state matches what is shown; add a DOM test. (3) FE10: "1.0" versus "1" counts as edited (assets-state.ts ~line 364): compare numbers by value, not by text; add a test.' + FECOMMON,
  allowedPaths: ['^package\.json$', '^package-lock\.json$', '^README\.md$', '^src/', '^scripts/'],
  acceptance: ['No-op Apply never silently keeps an invalid layer matrix', 'Invalid number draft does not block Apply after switching to JSON view', '1.0 vs 1 is not an edit; typecheck, tests and 3 CI checks green'],
  reviewer: 'sonnet',
  notes: 'Reviewer: each of the three findings fixed at the cited code with a test; no behaviour change elsewhere in the settings/assets panels.',
}
fs.writeFileSync('C:/Method Notes Obsidian/orchestrator/items/' + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log(it.name)
