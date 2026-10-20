// Reports what the vendored Jolt Physics weighs in a web build, for the decision to link it into the player
// (docs/web-build.html, "Jolt Physics: measured size"). It only reports: no budget is checked here, and nothing is
// linked into the player package.
// Usage: node scripts/web-jolt-size-report.mjs <build folder> <profile>
//   <build folder> is a configured and built web tree (N2ENGINE_USE_JOLT=ON) in which the targets jolt_size_baseline and
//   jolt_size_probe were built and the web-package target was built; <profile> only names the table.
// Two numbers, both measured on the same profile: (1) the static library Jolt (lib/libJolt.a, an archive of objects,
// before the linker drops what is not used), (2) the .wasm of jolt_size_probe (the smoke test's scenario, no test framework)
// minus the .wasm of jolt_size_baseline (a program that prints a line), raw, gzip -9 and brotli -q 11 (Node's zlib, as
// scripts/web-package-finalize.mjs) and as a share of the player package's index.wasm from its manifest.json.
// Prints a table and appends it to the job summary when GITHUB_STEP_SUMMARY is set. Exit 2 when an input is missing.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const dir = path.resolve(process.argv[2] ?? '');
const profile = process.argv[3] ?? '?';
const baselinePath = path.join(dir, 'bin', 'jolt_size_baseline.wasm');
const probePath = path.join(dir, 'bin', 'jolt_size_probe.wasm');
const libraryPath = path.join(dir, 'lib', 'libJolt.a');
const manifestPath = path.join(dir, 'web-package', 'manifest.json');
for (const file of [baselinePath, probePath, libraryPath, manifestPath]) {
  if (!fs.existsSync(file)) {
    console.error(`missing ${file}`);
    console.error('usage: web-jolt-size-report.mjs <build folder> <profile>');
    process.exit(2);
  }
}

const gzip = (buffer) => zlib.gzipSync(buffer, { level: 9 }).length;
const brotli = (buffer) =>
  zlib.brotliCompressSync(buffer, {
    params: {
      [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
      [zlib.constants.BROTLI_PARAM_LGWIN]: 24,
      [zlib.constants.BROTLI_PARAM_SIZE_HINT]: buffer.length,
    },
  }).length;
const measure = (file) => {
  const buffer = fs.readFileSync(file);
  return { raw: buffer.length, gzip: gzip(buffer), brotli: brotli(buffer) };
};

const baseline = measure(baselinePath);
const probe = measure(probePath);
const library = measure(libraryPath);
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const player = manifest.files?.['index.wasm'];
const delta = { raw: probe.raw - baseline.raw, gzip: probe.gzip - baseline.gzip, brotli: probe.brotli - baseline.brotli };

const fmt = (n) => (typeof n === 'number' ? n.toLocaleString('en-US') : 'n/a');
const share = (value, whole) => (typeof whole === 'number' && whole > 0 ? `${((100 * value) / whole).toFixed(1)} %` : 'n/a');
const rows = [
  ['libJolt.a (archive, before linking)', library],
  ['jolt_size_baseline.wasm (prints a line)', baseline],
  ['jolt_size_probe.wasm (baseline + Jolt scenario)', probe],
  ['Jolt delta (probe - baseline)', delta],
];
if (player) rows.push(['player package index.wasm (no Jolt)', { raw: player.raw, gzip: player.gzip, brotli: player.brotli }]);

const lines = [];
lines.push(`### Jolt Physics size, web ${profile} profile (not linked into the player)`);
lines.push('');
lines.push('| what | raw bytes | gzip -9 bytes | brotli -q 11 bytes |');
lines.push('|---|---:|---:|---:|');
for (const [name, v] of rows) lines.push(`| ${name} | ${fmt(v.raw)} | ${fmt(v.gzip)} | ${fmt(v.brotli)} |`);
if (player) {
  lines.push('');
  lines.push(
    `Jolt delta as a share of the player's index.wasm: raw ${share(delta.raw, player.raw)}, gzip ${share(delta.gzip, player.gzip)}, brotli ${share(delta.brotli, player.brotli)}.`
  );
}
const text = lines.join('\n');
console.log(text);
if (process.env.GITHUB_STEP_SUMMARY) fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, `${text}\n\n`);
