// Finishes the web package folder after emcc has linked it (the web-package target runs this).
// Usage: node scripts/web-package-finalize.mjs <package folder> --loader <n2-loader.js> --profile dev|size|speed [--precompress]
//
//   * copies the loader (web/n2-loader.js) next to index.html
//   * versions the page's script tags: index.js?v=<hash> and n2-loader.js?v=<hash> (first 12 hex digits of sha256)
//   * with --precompress: writes index.<ext>.br and index.<ext>.gz next to each compressible file (the plain files stay:
//     the package works on a host that does neither); stale .br/.gz files are removed first either way
//   * writes manifest.json: for every file of the package, its raw size, sha256, gzip size and brotli size (brotli is
//     null when it was not computed: the dev profile's wasm is a few hundred MB of DWARF and brotli -q 11 on it is slow)
// Node's zlib does the compression (gzip -9, brotli quality 11): no extra tool or module to pin, and the output is
// reproducible (no timestamps).
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const args = process.argv.slice(2);
const dir = path.resolve(args[0] ?? '');
function option(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}
const loader = option('--loader');
const profile = option('--profile') ?? 'dev';
const precompress = args.includes('--precompress');
if (!fs.existsSync(dir) || !loader || !fs.existsSync(loader)) {
  console.error('usage: web-package-finalize.mjs <package folder> --loader <n2-loader.js> [--profile dev|size|speed] [--precompress]');
  process.exit(2);
}

const compressible = new Set(['.html', '.js', '.wasm', '.data']);
for (const name of fs.readdirSync(dir)) {
  if (/\.(br|gz)$/.test(name) || name === 'manifest.json') fs.rmSync(path.join(dir, name));
}
fs.copyFileSync(loader, path.join(dir, 'n2-loader.js'));

// The page's two script tags get ?v=<first 12 hex digits of the file's sha256>, the same version the loader puts on
// index.wasm and index.data, so a fresh index.html never runs an older cached index.js or loader. Idempotent: an
// existing ?v= is replaced. emcc may have minified the page (unquoted attribute values): both forms are matched.
const sha256 = (buffer) => crypto.createHash('sha256').update(buffer).digest('hex');
const pagePath = path.join(dir, 'index.html');
let page = fs.readFileSync(pagePath, 'utf8');
for (const script of ['index.js', 'n2-loader.js']) {
  const version = sha256(fs.readFileSync(path.join(dir, script))).slice(0, 12);
  const escaped = script.replace(/[.]/g, '\\.');
  // The new value is always quoted (an unquoted value with '=' in it is an HTML parse error)
  const tag = new RegExp(`(<script\\b[^>]*\\bsrc\\s*=\\s*)(["']?)${escaped}(?:\\?v=[0-9a-f]*)?\\2(?=[\\s>])`, 'g');
  const matches = page.match(tag) ?? [];
  if (matches.length !== 1) {
    console.error(`web-package-finalize: index.html has ${matches.length} script tags for ${script}, expected 1`);
    process.exit(1);
  }
  page = page.replace(tag, `$1"${script}?v=${version}"`);
}
fs.writeFileSync(pagePath, page);

const gzip = (buffer) => zlib.gzipSync(buffer, { level: 9 });
const brotli = (buffer) => zlib.brotliCompressSync(buffer, {
  params: {
    [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
    [zlib.constants.BROTLI_PARAM_LGWIN]: 24,
    [zlib.constants.BROTLI_PARAM_SIZE_HINT]: buffer.length,
  },
});

const files = {};
for (const name of fs.readdirSync(dir).sort()) {
  const full = path.join(dir, name);
  if (!fs.statSync(full).isFile()) continue;
  const buffer = fs.readFileSync(full);
  const gz = gzip(buffer);
  // Brotli is computed for the profiles that ship it; the dev profile's size is reported by gzip only
  const br = precompress ? brotli(buffer) : null;
  files[name] = {
    raw: buffer.length,
    sha256: sha256(buffer),
    gzip: gz.length,
    brotli: br ? br.length : null,
  };
  if (precompress && compressible.has(path.extname(name))) {
    fs.writeFileSync(`${full}.gz`, gz);
    fs.writeFileSync(`${full}.br`, br);
  }
}

const manifest = {
  format: 1,
  profile,
  precompressed: precompress,
  files,
};
fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(`web package ${dir}: ${Object.keys(files).length} files, profile ${profile}, ${precompress ? 'with .br/.gz siblings' : 'no precompressed siblings'}, manifest.json written`);
