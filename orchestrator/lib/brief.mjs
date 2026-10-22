// node brief.mjs <item>  : write the developer and reviewer briefs; print {ok} or {blocked}.
import fs from "node:fs"
import { ORCH, REPOS, ledgerDir, loadItem, out } from "./common.mjs"

const name = process.argv[2]
const item = loadItem(name)
if (item.gate && !item.approved) { out({ name, blocked: item.gate }); process.exit(0) }

const L = ledgerDir(name)
fs.mkdirSync(L, { recursive: true })
const repo = REPOS[item.repo]
const rules = `## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through \`bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>\`, run from inside your worktree. Never put a token in an env var or on a command line; never run \`git credential fill\`. If any access is denied, STOP and report it.
- Push with \`git push origin HEAD:refs/heads/${item.branch}\` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run \`git status --ignored\` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.`

const dev = `# Task ${name}: ${item.title}
Repo: ${item.repo} (${repo.path}, ${repo.slug}). Branch: ${item.branch}.${item.closes ? ` Closes #${item.closes}.` : ""}

## Issue / plan
${item.issue}

## Allowed paths (regex, repo-relative)
${item.allowedPaths.map((p) => "- " + p).join("\n")}

## Acceptance
${item.acceptance.map((a) => "- " + a).join("\n")}
${item.requiresDocs ? "- docs/ updated in the same PR (PR #N badge, design-decision callouts).\n" : ""}${item.notes ? "\n## Notes\n" + item.notes + "\n" : ""}
## Procedure
1. \`node ${ORCH}/lib/setup.mjs ${name}\` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. ${item.repo === "engine" ? "Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref " + item.branch + "` via the wrapper if useful." : "Run npm run typecheck, npm run build, npm test locally."}
4. Open the PR from a body file in ${ORCH}/ledger/${name}/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check \`pr list --head ${item.branch} --state all\` before retrying.
5. Write ${L}/dev.json: {"pr":<number>,"branch":"${item.branch}","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

${rules}
`
const review = `# Review ${name}: ${item.title}
Read-only. Do NOT edit files, push, or comment on GitHub. Repo ${repo.path}. First \`git fetch\`, then review \`git diff origin/master...origin/${item.branch}\` (read ${L}/dev.json for the PR number and the dev's deviations; if ${L}/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: \`bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...\` from inside ${repo.path}.

## Plan under review
${item.issue}

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; ${item.repo === "engine" ? "GL/GPU code CI can't run (say so);" : "line endings;"} regressions. Say nothing about style.

## Output
Overwrite ${L}/review.json: {"head":"<origin/${item.branch} sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
`
fs.writeFileSync(`${L}/brief-dev.md`, dev)
fs.writeFileSync(`${L}/brief-review.md`, review)
out({ name, ok: true })
