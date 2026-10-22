// Shared helpers for the orchestrator scripts. Zero dependencies (Node 22).
import { spawnSync } from "node:child_process"
import fs from "node:fs"
import path from "node:path"

export const ORCH = "C:/Method Notes Obsidian/orchestrator"
export const WRAPPER = "C:/Method Notes Obsidian/gh-epicnicks.sh"
export const REPOS = {
  engine: { path: "C:/Method Notes Obsidian/N2Engine", slug: "EpicNicks/N2Engine" },
  frontend: { path: "C:/Method Notes Obsidian/N2Engine-Electron-Frontend", slug: "EpicNicks/N2Engine-Electron-Frontend" },
}

export const ledgerDir = (name) => `${ORCH}/ledger/${name}`
export const readJson = (file, fallback = null) => {
  try { return JSON.parse(fs.readFileSync(file, "utf8")) } catch { return fallback }
}
export const writeJson = (file, value) => {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n")
}
export function loadItem(name) {
  const item = readJson(`${ORCH}/items/${name}.json`)
  if (!item) throw new Error(`no item file items/${name}.json`)
  if (!REPOS[item.repo]) throw new Error(`item ${name}: repo must be engine or frontend`)
  return item
}

// gh only through the wrapper, from inside the repo (it reads the per-repo credential itself).
export function gh(repoKey, args, cwd) {
  const r = spawnSync("bash", [WRAPPER, ...args], { cwd: cwd ?? REPOS[repoKey].path, encoding: "utf8", maxBuffer: 256 << 20 })
  return { out: r.stdout ?? "", err: r.stderr ?? "", status: r.status }
}
export function ghJson(repoKey, args, cwd) {
  const r = gh(repoKey, args, cwd)
  if (!r.out.trim()) throw new Error(`gh ${args.join(" ")}: empty output (${r.err.trim().slice(0, 200)})`)
  return JSON.parse(r.out)
}
export function git(repoKey, args, cwd) {
  const r = spawnSync("git", args, { cwd: cwd ?? REPOS[repoKey].path, encoding: "utf8", maxBuffer: 256 << 20 })
  return { out: r.stdout ?? "", err: r.stderr ?? "", status: r.status }
}
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
export const out = (value) => console.log(JSON.stringify(value))
