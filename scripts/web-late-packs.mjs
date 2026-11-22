#!/usr/bin/env node
// The late packs of a web package (#425, docs/web-build.html#late-packs): for each folder a project lists in
// build.web.lateAssets, a stored zip late-<n>.n2pack in the package folder, holding that folder's files (paths relative to
// the folder, where the player mounts it). The folder itself is left out of index.data by the link (web/CMakeLists.txt),
// late-packs.json (written by CMake, preloaded at /project/late-packs.json) tells the player which folders wait, and the
// loader (web/n2-loader.js) fetches the packs named in manifest.json's latePacks once the game runs.
//
// Usage: web-late-packs.mjs --project DIR --out PACKAGE_DIR --folders "assets/a,assets/b"
// Same project, same bytes (writeZip's fixed order and time). Exit 1 on a folder that is missing or outside assets/.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { writeZip } = await import(new URL('../tools/n2build/lib/cook.mjs', import.meta.url).href);

export function parseFolders(text) {
  return text.split(',').map((f) => f.trim().replace(/\\/g, '/').replace(/\/+$/, '')).filter((f) => f.length > 0);
}

/** Why `folder` (project-relative) cannot be a late folder, or null */
export function folderProblem(projectDir, folder) {
  const parts = folder.split('/');
  if (parts[0] !== 'assets' || parts.length < 2 || parts.some((p) => p === '' || p === '.' || p === '..')) {
    return `${folder}: a late folder is a folder inside assets/ (for example assets/levels)`;
  }
  const full = path.join(projectDir, ...parts);
  if (!fs.existsSync(full) || !fs.statSync(full).isDirectory()) return `${folder}: no such folder in ${projectDir}`;
  return null;
}

/** Every file under dir as {name (relative, '/'-separated), data}, sorted by name */
export function folderEntries(dir) {
  const out = [];
  const walk = (at, prefix) => {
    for (const name of fs.readdirSync(at).sort()) {
      const full = path.join(at, name);
      const rel = prefix ? `${prefix}/${name}` : name;
      if (fs.statSync(full).isDirectory()) walk(full, rel);
      else out.push({ name: rel, data: fs.readFileSync(full) });
    }
  };
  walk(dir, '');
  return out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

/** Writes the packs; returns [{file, folder, files, bytes}] */
export function writeLatePacks(projectDir, outDir, folders) {
  const packs = [];
  // Overlapping folders would mount one inside the other: refused
  for (const a of folders) {
    for (const b of folders) {
      if (a !== b && (b + '/').startsWith(a + '/')) throw new Error(`late folders ${a} and ${b} overlap`);
    }
  }
  folders.forEach((folder, index) => {
    const problem = folderProblem(projectDir, folder);
    if (problem) throw new Error(problem);
    const entries = folderEntries(path.join(projectDir, ...folder.split('/')));
    const zip = writeZip(entries, { stored: true });
    const file = `late-${index}.n2pack`;
    fs.writeFileSync(path.join(outDir, file), zip);
    packs.push({ file, folder, files: entries.length, bytes: zip.length });
  });
  return packs;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const get = (name) => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : undefined;
  };
  const projectDir = get('--project');
  const outDir = get('--out');
  const folders = parseFolders(get('--folders') ?? '');
  if (!projectDir || !outDir) {
    console.error('usage: web-late-packs.mjs --project DIR --out PACKAGE_DIR --folders "assets/a,assets/b"');
    process.exit(2);
  }
  // Stale packs of an earlier build go first, so the package holds exactly this build's
  for (const name of fs.readdirSync(outDir)) {
    if (/^late-\d+\.n2pack(\.br|\.gz)?$/.test(name)) fs.rmSync(path.join(outDir, name));
  }
  try {
    for (const pack of writeLatePacks(projectDir, outDir, folders)) {
      console.log(`late pack ${pack.file}: ${pack.folder}, ${pack.files} files, ${pack.bytes} bytes`);
    }
  } catch (error) {
    console.error('web-late-packs: ' + error.message);
    process.exit(1);
  }
  void here;
}
