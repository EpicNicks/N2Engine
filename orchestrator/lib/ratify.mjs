// node ratify.mjs <item> [--merge] : the deterministic gate. Prints {decision: merge|hold, reasons, merged?}.
import fs from "node:fs"
import { REPOS, gh, ghJson, git, ledgerDir, loadItem, out, readJson, writeJson } from "./common.mjs"
import { ciOnce } from "./ci.mjs"

const name = process.argv[2]
const doMerge = process.argv.includes("--merge")
const item = loadItem(name)
const L = ledgerDir(name)
const repo = REPOS[item.repo]
const reasons = []
let relaxedNote = null
const hold = (r) => reasons.push(r)
const finish = (result) => { writeJson(`${L}/ratify.json`, result); out(result); process.exit(0) }

const state = readJson(`${L}/state.json`, {})
const dev = readJson(`${L}/dev.json`)
const review = readJson(`${L}/review.json`)
if (state.merged) finish({ name, decision: "hold", reasons: ["already merged"] })
if (!dev?.pr) finish({ name, decision: "hold", reasons: ["no dev.json / pr"] })

if (dev.needsUser) hold(`dev needs the user: ${dev.needsUser}`)
if (dev.deviations?.length) hold(`deviations from the plan need the user: ${dev.deviations.join(" | ").slice(0, 300)}`)

let pr
try { pr = ghJson(item.repo, ["api", `repos/${repo.slug}/pulls/${dev.pr}`]) } catch (e) { finish({ name, decision: "hold", reasons: [`cannot read PR: ${e.message}`] }) }
const head = pr.head.sha
if (pr.state !== "open") hold(`PR is ${pr.state}`)

// The review must be of the current head, with no blocker/high findings.
if (!review) hold("no review.json")
else {
  if (review.head !== head) hold(`review is of ${String(review.head).slice(0, 7)}, PR head is ${head.slice(0, 7)}`)
  const bad = (review.findings ?? []).filter((f) => f.severity === "blocker" || f.severity === "high")
  if (bad.length) hold(`${bad.length} open blocker/high finding(s)`)
}

// CI green on this exact head.
try {
  const ci = ciOnce(item, dev.pr)
  if (!ci.done) hold("CI not finished")
  else if (!ci.allGreen) hold("CI not green")
} catch (e) { hold(`cannot read CI: ${e.message}`.slice(0, 200)) }

// Current with master, and diff hygiene.
git(item.repo, ["fetch", "-q"])
const br = `origin/${item.branch}`
if (git(item.repo, ["rev-parse", "--verify", "-q", br]).status !== 0) hold(`branch ${br} not found`)
else {
  if (git(item.repo, ["merge-base", "--is-ancestor", "origin/master", br]).status !== 0) {
    // Relaxed by the user (2026-10-15): a branch that is behind master may merge when master's new
    // commits touch NONE of the PR's files and a trial merge is conflict-free; master's push CI verifies the result.
    const base = git(item.repo, ["merge-base", "origin/master", br]).out.trim()
    const prFiles = new Set(git(item.repo, ["diff", "--name-only", `${base}..${br}`]).out.split("\n").filter(Boolean))
    const masterFiles = git(item.repo, ["diff", "--name-only", `${base}..origin/master`]).out.split("\n").filter(Boolean)
    const overlapAll = masterFiles.filter((f) => prFiles.has(f))
    const overlap = overlapAll.filter((f) => !/^docs\/[^/]+\.html$/.test(f)) // docs/*.html overlap is allowed (user, 2026-10-15) when the trial merge is clean
    const trial = git(item.repo, ["merge-tree", "--write-tree", "origin/master", br])
    if (overlap.length) hold(`branch is behind master and master changed the PR's files: ${overlap.slice(0, 5).join(", ")}`)
    else if (trial.status !== 0) hold("branch is behind master and a trial merge conflicts")
    else relaxedNote = `behind master by ${masterFiles.length} file(s), no code overlap with the PR's files${overlapAll.length ? ` (docs overlap: ${overlapAll.join(', ')})` : ""}, trial merge clean`
  }
  const files = git(item.repo, ["diff", "--name-only", `origin/master...${br}`]).out.split("\n").filter(Boolean)
  const res = item.allowedPaths.map((p) => new RegExp(p))
  const stray = files.filter((f) => !res.some((r) => r.test(f)))
  if (stray.length) hold(`files outside the allowed paths: ${stray.slice(0, 5).join(", ")}`)
  if (item.requiresDocs && !files.some((f) => f.startsWith("docs/"))) hold("no docs/ change")
  if (!item.allowProtocol && files.some((f) => /(^|\/)protocol\.json$/.test(f))) hold("touches protocol.json (needs the user)")
  const added = git(item.repo, ["diff", `origin/master...${br}`]).out.split("\n").filter((l) => l.startsWith("+") && !l.startsWith("+++")).join("\n")
  if (/(ghp_|gho_|github_pat_)[A-Za-z0-9_]{10,}|-----BEGIN [A-Z ]*PRIVATE KEY/.test(added)) hold("a token or private key appears in the diff")
  const msgs = git(item.repo, ["log", `origin/master..${br}`, "--format=%B"]).out
  if (/co-authored-by|generated with \[?claude/i.test(msgs + "\n" + (pr.body ?? ""))) hold("attribution in a commit message or the PR body")
}
if (dev.unverified?.length && !/not verified|unverified/i.test(pr.body ?? "")) hold("unverified items are not listed in the PR body")

const result = { name, decision: reasons.length ? "hold" : "merge", reasons, ...(relaxedNote ? { relaxed: relaxedNote } : {}) }
if (result.decision === "merge" && doMerge) {
  // The PR body gets a "## Review" section built from the ledger.
  if (!/^## Review/m.test(pr.body ?? "")) {
    const rest = (review.findings ?? []).filter((x) => x.severity === "medium" || x.severity === "low")
    const section = `\n\n## Review\n\nAn independent review of ${head.slice(0, 7)} found no open blocker or high findings; CI is green on all jobs.` +
      (rest.length ? ` Remaining notes: ${rest.map((x) => `${x.file}:${x.line} ${x.summary}`).join("; ")}.` : "") + "\n"
    const file = `${L}/pr-body.md`
    fs.writeFileSync(file, (pr.body ?? "") + section)
    gh(item.repo, ["api", "-X", "PATCH", `repos/${repo.slug}/pulls/${dev.pr}`, "-F", `body=@${file}`])
    fs.rmSync(file, { force: true })
  }
  const m = ghJson(item.repo, ["api", "-X", "PUT", `repos/${repo.slug}/pulls/${dev.pr}/merge`, "-f", "merge_method=merge"])
  result.merged = !!m.merged
  if (m.merged) {
    gh(item.repo, ["api", "-X", "DELETE", `repos/${repo.slug}/git/refs/heads/${item.branch}`])
    if (state.worktree) {
      git(item.repo, ["worktree", "remove", "--force", "--force", state.worktree])
      if (fs.existsSync(state.worktree)) { try { fs.rmSync(state.worktree, { recursive: true, force: true }) } catch {} }
    }
    git(item.repo, ["worktree", "prune"])
    git(item.repo, ["branch", "-D", item.branch])
    for (const b of git(item.repo, ["branch", "--list", "worktree-agent-*"]).out.split("\n").map((s) => s.trim().replace(/^[*+]\s*/, "")).filter(Boolean)) git(item.repo, ["branch", "-D", b])
    git(item.repo, ["fetch", "-q"])
    if (git(item.repo, ["rev-parse", "--abbrev-ref", "HEAD"]).out.trim() === "master" && !git(item.repo, ["status", "--porcelain"]).out.trim()) git(item.repo, ["merge", "-q", "--ff-only", "origin/master"])
    writeJson(`${L}/state.json`, { ...state, merged: true, pr: dev.pr })
  } else {
    result.decision = "hold"
    result.reasons.push("merge API refused")
  }
}
finish(result)
