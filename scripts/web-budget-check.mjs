// Checks a web package's manifest.json against the size budgets in web/budgets.json, the one place budgets live.
// Usage: node scripts/web-budget-check.mjs <package folder> <profile> [budgets.json]
// Prints a table (and appends it to the job summary when GITHUB_STEP_SUMMARY is set); exits 1 when any budget is exceeded
// or a budgeted number is missing from the manifest.
//
// A budget key is "<kind>.<size>": kind is wasm, js, data or initial (initial = every file of the package, i.e. what a
// first visit downloads, manifest.json excluded), size is raw, gzip or brotli. Values are bytes, upper limits.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dir = path.resolve(process.argv[2] ?? '');
const profile = process.argv[3];
const budgetFile = path.resolve(process.argv[4] ?? path.join(here, '..', 'web', 'budgets.json'));
if (!profile || !fs.existsSync(path.join(dir, 'manifest.json'))) {
  console.error('usage: web-budget-check.mjs <package folder with manifest.json> <profile> [budgets.json]');
  process.exit(2);
}
const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
const budgets = JSON.parse(fs.readFileSync(budgetFile, 'utf8')).profiles?.[profile];
if (!budgets) {
  console.error(`${budgetFile} has no budgets for the profile '${profile}'`);
  process.exit(2);
}

function measure(kind, size) {
  const entries = Object.entries(manifest.files).filter(([name]) => (kind === 'initial' ? true : name.endsWith(`.${kind}`)));
  if (entries.length === 0) return null;
  let sum = 0;
  for (const [, entry] of entries) {
    if (typeof entry[size] !== 'number') return null;
    sum += entry[size];
  }
  return sum;
}

const rows = [];
let failed = false;
for (const [key, limit] of Object.entries(budgets)) {
  const [kind, size] = key.split('.');
  const value = measure(kind, size);
  let state = 'ok';
  if (value === null) {
    state = 'MISSING';
    failed = true;
  } else if (value > limit) {
    state = 'OVER';
    failed = true;
  }
  const headroom = value === null ? '' : `${(((limit - value) / limit) * 100).toFixed(1)}%`;
  rows.push(`| ${key} | ${value ?? 'n/a'} | ${limit} | ${headroom} | ${state} |`);
}

const lines = [
  `### Web size budgets (profile ${profile}, ${path.relative(process.cwd(), budgetFile) || budgetFile})`,
  '',
  '| budget | measured bytes | limit bytes | headroom | |',
  '|---|---:|---:|---:|---|',
  ...rows,
  '',
  failed ? '**A budget is exceeded or its number is missing.** Raise it in web/budgets.json only with a reason in the PR.' : 'All budgets met.',
];
console.log(lines.join('\n'));
if (process.env.GITHUB_STEP_SUMMARY) fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, lines.join('\n') + '\n');
process.exit(failed ? 1 : 0);
