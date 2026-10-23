// usage: node accept.js <item> <repoSlug> <pr>
const fs = require("fs")
const { execSync } = require("child_process")
const [item, slug, pr] = process.argv.slice(2)
const dir = `C:/Method Notes Obsidian/orchestrator/ledger/${item}/`
const d = JSON.parse(fs.readFileSync(dir + "dev.json", "utf8"))
const devs = (d.deviations || []).filter((t) => !/Co-Authored-By|Generated with|harness reminder/i.test(t))
let body = fs.readFileSync(dir + "pr-body.md", "utf8")
if (!/## Approved deviations/.test(body)) body = body.trimEnd() + "\n\n## Approved deviations\nAccepted by the user 2026-10-13:\n" + devs.map((t) => "- " + t).join("\n") + "\n"
fs.writeFileSync(dir + "pr-body.md", body)
fs.writeFileSync(dir + "notes.md", "Accepted by the user 2026-10-13:\n" + devs.map((t) => "- " + t).join("\n") + "\n")
d.deviations = []
fs.writeFileSync(dir + "dev.json", JSON.stringify(d))
const cwd = slug === "frontend" ? "C:/Method Notes Obsidian/N2Engine-Electron-Frontend" : "C:/Method Notes Obsidian/N2Engine"
const repo = slug === "frontend" ? "N2Engine-Electron-Frontend" : "N2Engine"
console.log(execSync(`bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api -X PATCH repos/EpicNicks/${repo}/pulls/${pr} -F body=@"${dir}pr-body.md" --jq .number`, { cwd }).toString())
console.log(execSync(`node ../orchestrator/lib/ratify.mjs ${item} --merge`, { cwd: "C:/Method Notes Obsidian/N2Engine" }).toString())
