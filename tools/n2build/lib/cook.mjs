// The cook step (build targets stage 4; docs/packs.html): a project folder becomes one pack, the file a player mounts where
// the game's folder would be. In order:
//   1. the module-list check for the target (scripts/project-modules.mjs), so a game cannot ship without a module it uses;
//   2. the files a game needs: project.n2proj, assets/ and .import/ (the stored asset UUIDs and import settings);
//   3. a manifest of every file's size and SHA-256, inside the pack (n2pack-manifest.json) and beside it;
//   4. the pack: zip (deflate, or stored where deflate saves nothing), stored zip, or 7z written non-solid by 7-Zip, so a
//      player reads one file without decoding the others.
// A zip is written here, in a fixed order with a fixed time, so the same project always makes the same bytes.
import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';

import { findConflicts, scanProject } from '../../../scripts/project-modules.mjs';
import { BuildError } from './report.mjs';
import { findOnPath } from './toolchains.mjs';

export const PACK_FORMATS = ['zip', 'stored', '7z'];
export const PACK_EXTENSION = '.n2pack';
export const MANIFEST_NAME = 'n2pack-manifest.json';
/** What a pack holds of a project folder */
export const PACK_ROOTS = { files: ['project.n2proj'], folders: ['assets', '.import'] };

const ZIP_LIMIT = 0xfffffffe;

/** The project's files a pack holds, as {name ('/'-separated), file (on disk)}, sorted by name */
export function packFiles(projectDir) {
  const out = [];
  for (const name of PACK_ROOTS.files) {
    const file = path.join(projectDir, name);
    if (fs.existsSync(file)) out.push({ name, file });
  }
  const walk = (dir, prefix) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))) {
      const file = path.join(dir, entry.name);
      const name = `${prefix}/${entry.name}`;
      if (entry.isDirectory()) walk(file, name);
      else if (entry.isFile()) out.push({ name, file });
    }
  };
  for (const folder of PACK_ROOTS.folders) {
    const dir = path.join(projectDir, folder);
    if (fs.existsSync(dir) && fs.statSync(dir).isDirectory()) walk(dir, folder);
  }
  for (const { name } of out) {
    if (name.includes('\\') || name.split('/').some((part) => part === '' || part === '.' || part === '..')) {
      throw new BuildError(`${name} can't go in a pack: its name is not a plain path`);
    }
  }
  return out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

/** The manifest: every file's size and SHA-256, and the pack's own description */
export function makeManifest(files, { target, format, game }) {
  let totalSize = 0;
  const entries = files.map(({ name, data }) => {
    totalSize += data.length;
    return { path: name, size: data.length, sha256: crypto.createHash('sha256').update(data).digest('hex') };
  });
  return { formatVersion: 1, target, format, game, fileCount: entries.length, totalSize, files: entries };
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(data) {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

/** A zip of {name, data} entries: deflate where it saves bytes (unless stored), else stored. Returns a Buffer. */
export function writeZip(entries, { stored = false } = {}) {
  if (entries.length > 0xfffe) throw new BuildError(`A zip pack holds at most 65534 files; this one has ${entries.length}`);
  // 1980-01-01 00:00, the zip epoch: a fixed time, so the same project makes the same pack
  const dosTime = 0;
  const dosDate = (0 << 9) | (1 << 5) | 1;
  const locals = [];
  const central = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBytes = Buffer.from(name, 'utf8');
    let method = 0;
    let body = data;
    if (!stored && data.length > 0) {
      const deflated = zlib.deflateRawSync(data, { level: 9 });
      if (deflated.length < data.length) {
        method = 8;
        body = deflated;
      }
    }
    if (data.length > ZIP_LIMIT || offset > ZIP_LIMIT) throw new BuildError(`The pack would be over 4 GiB (${name})`);
    const crc = crc32(data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6); // UTF-8 names
    local.writeUInt16LE(method, 8);
    local.writeUInt16LE(dosTime, 10);
    local.writeUInt16LE(dosDate, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(body.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    local.writeUInt16LE(0, 28);
    locals.push(local, nameBytes, body);

    const header = Buffer.alloc(46);
    header.writeUInt32LE(0x02014b50, 0);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(20, 6);
    header.writeUInt16LE(0x0800, 8);
    header.writeUInt16LE(method, 10);
    header.writeUInt16LE(dosTime, 12);
    header.writeUInt16LE(dosDate, 14);
    header.writeUInt32LE(crc, 16);
    header.writeUInt32LE(body.length, 20);
    header.writeUInt32LE(data.length, 24);
    header.writeUInt16LE(nameBytes.length, 28);
    header.writeUInt32LE(offset, 42);
    central.push(header, nameBytes);
    offset += 30 + nameBytes.length + body.length;
  }
  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, directory, end]);
}

/** 7-Zip's command line (7z, 7za or 7zz on PATH, or 7-Zip's own folder on Windows), or null */
export function findSevenZip(env = process.env, ctx = { platform: process.platform, exists: (file) => fs.existsSync(file) }) {
  for (const name of ['7z', '7za', '7zz']) {
    const found = findOnPath(name, env, ctx);
    if (found) return found;
  }
  if (ctx.platform === 'win32') {
    for (const base of [env.ProgramFiles, env['ProgramFiles(x86)'], 'C:\\Program Files']) {
      if (!base) continue;
      const exe = path.win32.join(base, '7-Zip', '7z.exe');
      if (ctx.exists(exe)) return exe;
    }
  }
  return null;
}

/** A non-solid 7z of the entries, through 7-Zip (one block per file: a player decodes only the file it reads) */
export function writeSevenZip(entries, outFile, { sevenZip, env = process.env }) {
  if (!sevenZip) throw new BuildError('A 7z pack needs 7-Zip\'s command line (7z, 7za or 7zz): install 7-Zip, or choose the zip pack');
  const staging = fs.mkdtempSync(path.join(os.tmpdir(), 'n2pack-'));
  try {
    for (const { name, data } of entries) {
      const file = path.join(staging, ...name.split('/'));
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, data);
    }
    fs.rmSync(outFile, { force: true });
    // -ms=off: non-solid; LZMA2, no filters; -mtm=off: no stored times (the same project makes the same pack)
    const args = ['a', '-t7z', '-m0=LZMA2', '-ms=off', '-mx=7', '-mtm=off', '-mtc=off', '-mta=off', '-bd', '-y', path.resolve(outFile), '.'];
    const result = spawnSync(sevenZip, args, { cwd: staging, env, encoding: 'utf8', windowsHide: true });
    if (result.error || result.status !== 0) {
      throw new BuildError(`7-Zip could not write ${outFile}: ${result.error?.message ?? (result.stderr || result.stdout).trim()}`);
    }
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
  }
}

/**
 * Cooks projectDir into outFile. format: zip, stored or 7z. target: for the module check (the target's own list) and the
 * manifest. Returns the manifest.
 */
export function cookPack({ projectDir, outFile, format, target, game, sevenZip, env = process.env, skipModuleCheck = false }) {
  if (!PACK_FORMATS.includes(format)) throw new BuildError(`Unknown pack format '${format}' (${PACK_FORMATS.join(', ')})`);
  if (!skipModuleCheck) {
    let scan;
    try {
      scan = scanProject(projectDir, target);
    } catch (error) {
      throw new BuildError(`The project can't be checked: ${error.message}`);
    }
    const conflicts = findConflicts(scan);
    if (conflicts.length > 0) {
      throw new BuildError(`The engine modules do not fit the project:\n${conflicts.map((c) => `  ${c}`).join('\n')}`);
    }
  }
  const files = packFiles(projectDir).map(({ name, file }) => ({ name, data: fs.readFileSync(file) }));
  if (!files.some((f) => f.name === 'project.n2proj')) throw new BuildError(`${projectDir} has no project.n2proj`);
  const manifest = makeManifest(files, { target, format, game });
  const manifestBytes = Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`);
  const entries = [...files, { name: MANIFEST_NAME, data: manifestBytes }].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  fs.mkdirSync(path.dirname(path.resolve(outFile)), { recursive: true });
  if (format === '7z') writeSevenZip(entries, outFile, { sevenZip, env });
  else fs.writeFileSync(outFile, writeZip(entries, { stored: format === 'stored' }));
  fs.writeFileSync(`${outFile}.manifest.json`, manifestBytes);
  return manifest;
}
