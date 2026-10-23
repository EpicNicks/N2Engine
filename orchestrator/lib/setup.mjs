// node setup.mjs <item> : create the item's worktree from origin/master and record it in state.json.
import path from "node:path"
import { REPOS, git, ledgerDir, loadItem, out, readJson, writeJson } from "./common.mjs"

const name = process.argv[2]
const item = loadItem(name)
const repo = REPOS[item.repo]
git(item.repo, ["fetch", "-q"])
const wt = path.posix.join(path.posix.dirname(repo.path), `${path.posix.basename(repo.path)}-${name}`)
const exists = git(item.repo, ["worktree", "list", "--porcelain"]).out.includes(wt)
if (!exists) {
  const r = git(item.repo, ["worktree", "add", wt, "-b", item.branch, "origin/master"])
  if (r.status !== 0) { out({ name, error: r.err.trim().slice(0, 300) }); process.exit(0) }
}
const state = readJson(`${ledgerDir(name)}/state.json`, {})
writeJson(`${ledgerDir(name)}/state.json`, { ...state, repo: item.repo, branch: item.branch, worktree: wt })
out({ name, worktree: wt, branch: item.branch })
