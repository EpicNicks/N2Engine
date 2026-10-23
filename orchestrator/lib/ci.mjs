// CI state for a PR head, from the Actions runs API (not the check rollup, which lags a fresh push).
import { REPOS, gh, ghJson } from "./common.mjs"

const GREEN = new Set(["success", "skipped", "neutral"])
const ERR = /error C\d+|error LNK|\[  FAILED  \]|error TS\d+|\bFAIL\b|AssertionError|Error:/

export function ciOnce(item, pr) {
  const slug = REPOS[item.repo].slug
  const head = ghJson(item.repo, ["api", `repos/${slug}/pulls/${pr}`]).head.sha
  const runs = ghJson(item.repo, ["api", `repos/${slug}/actions/runs?head_sha=${head}`]).workflow_runs
  const done = runs.length > 0 && runs.every((r) => r.status === "completed")
  const allGreen = done && runs.every((r) => GREEN.has(r.conclusion))
  return { head, runs, done, allGreen }
}

// Only the failing lines of the failing jobs, capped: a fix agent gets ~25 lines, never a whole log.
export function failureLines(item, runs) {
  const slug = REPOS[item.repo].slug
  const lines = []
  for (const run of runs.filter((r) => r.status === "completed" && !GREEN.has(r.conclusion))) {
    const jobs = ghJson(item.repo, ["api", `repos/${slug}/actions/runs/${run.id}/jobs`]).jobs
    for (const job of jobs.filter((j) => j.conclusion && !GREEN.has(j.conclusion))) {
      const log = gh(item.repo, ["api", "--allow-escape-sequences", `repos/${slug}/actions/jobs/${job.id}/logs`]).out
      const hits = log.split(/\r?\n/).filter((l) => ERR.test(l)).map((l) => l.replace(/^\S+\s/, "").slice(0, 220))
      lines.push(`[${job.name}] ${hits.length ? "" : "(failed; no error lines matched)"}`)
      lines.push(...hits.slice(0, 12))
    }
  }
  return lines.slice(0, 25)
}
