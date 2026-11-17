// The conformance corpus (renderer/shaders/user/corpus/): every positive file must cook through every stage, every negative
// must be rejected at the stage and for the reason expect.json names, so the CI job can fail at all. expect.json:
//   { "positive": [ { "file": "a.n2shader", "relaxed": false } ],
//     "negative": [ { "file": "b.n2shader", "expect": [ { "options": { "lint": false }, "stage": "tint", "match": "regex" } ] } ] }
// A file of the folder that expect.json does not list is an error (an unlisted file would prove nothing).
import fs from 'node:fs';
import path from 'node:path';
import { cookShader } from './cook.mjs';

/**
 * @returns {{ ok: boolean, lines: string[], counts: { positive: number, negative: number, skipped: number, failed: number } }}
 */
export function runCorpus(dir, { tools, requireTools = false, targets, log = () => {} } = {}) {
  const spec = JSON.parse(fs.readFileSync(path.join(dir, 'expect.json'), 'utf8'));
  const lines = [];
  const counts = { positive: 0, negative: 0, skipped: 0, failed: 0 };
  const say = (line) => { lines.push(line); log(line); };
  const fail = (line) => { counts.failed += 1; say(`  FAIL ${line}`); };

  const listed = new Set([...(spec.positive ?? []).map((entry) => entry.file), ...(spec.negative ?? []).map((entry) => entry.file)]);
  for (const file of fs.readdirSync(dir).filter((name) => name.endsWith('.n2shader')).sort()) {
    if (!listed.has(file)) fail(`${file} is in the corpus folder but not in expect.json`);
  }

  for (const entry of spec.positive ?? []) {
    const result = cookShader(fs.readFileSync(path.join(dir, entry.file), 'utf8'), {
      path: entry.file, tools, requireTools, targets, strict: !entry.relaxed,
    });
    if (!result.ok) {
      fail(`positive ${entry.file}: rejected at ${result.failedStage}: ${result.diagnostics.filter((d) => d.severity === 'error').map((d) => `${d.file ?? ''}:${d.line ?? ''} ${d.message}`).join(' | ')}`);
      continue;
    }
    counts.positive += 1;
    counts.skipped += result.skipped.length;
    const stages = Object.entries(result.manifest.validated).map(([target, passed]) => `${target}[${passed.join(',')}]`).join(' ');
    say(`  ok   positive ${entry.file} ${stages}${result.skipped.length ? ` (${result.skipped.length} stage(s) skipped: ${[...new Set(result.skipped.map((s) => s.reason.split(':').pop().trim()))].join('; ')})` : ''}`);
  }

  for (const entry of spec.negative ?? []) {
    const text = fs.readFileSync(path.join(dir, entry.file), 'utf8');
    for (const expectation of entry.expect ?? []) {
      const options = expectation.options ?? {};
      const label = `negative ${entry.file}${Object.keys(options).length ? ` ${JSON.stringify(options)}` : ''}`;
      const result = cookShader(text, {
        path: entry.file, tools, requireTools, targets, strict: !options.relaxed, lint: options.lint !== false,
      });
      if (result.ok) {
        // accepted only because a stage that could have rejected it did not run (a tool is not installed): no verdict
        if (result.skipped.length > 0) { counts.skipped += 1; say(`  skip ${label}: no verdict, ${result.skipped[0].reason}`); continue; }
        fail(`${label}: ACCEPTED an invalid shader (expected rejection at ${expectation.stage})`);
        continue;
      }
      const errors = result.diagnostics.filter((d) => d.severity === 'error');
      if (result.failedStage !== expectation.stage) {
        fail(`${label}: rejected at ${result.failedStage}, expected ${expectation.stage}: ${errors.map((d) => d.message).join(' | ')}`);
        continue;
      }
      if (!errors.some((d) => new RegExp(expectation.match, 'i').test(`${d.file ?? ''}:${d.line ?? ''} ${d.message}`))) {
        fail(`${label}: rejected at ${expectation.stage}, but no message matches /${expectation.match}/: ${errors.map((d) => d.message).join(' | ')}`);
        continue;
      }
      counts.negative += 1;
      const first = errors[0];
      say(`  ok   ${label} rejected at ${expectation.stage}: ${first.file ? `${first.file}:${first.line ?? ''} ` : ''}${first.message.slice(0, 110)}`);
    }
  }
  return { ok: counts.failed === 0, lines, counts };
}
