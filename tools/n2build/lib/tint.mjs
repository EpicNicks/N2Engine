// Tint for the shader cook (user shaders, stage 6; proposal section 11 Q12). The cook turns a .n2shader into WGSL through Tint,
// the WGSL compiler of Dawn, on the developer's machine (n2build, the editor host). Tint is a build tool: it is never shipped
// with a game or the editor, and THIRD_PARTY_NOTICES.md lists it under "Not shipped" (BSD-3-Clause, The Dawn & Tint Authors).
//
// Where Tint comes from, in order: the TINT environment variable, a `tint` on PATH, then the copy n2build keeps in the user's
// data folder, downloaded ONCE, from the pinned Dawn release (the same release scripts/install-wgsl-validators.sh pins for CI).
// The Windows archive is 119 MB for a 7.6 MB tint.exe, so the download reads the archive as a stream, takes the one entry and
// stops; what is kept is that binary alone, and it is used only when its SHA-256 is the pinned one (a pin is the hash of the
// binary, taken from the file with sha256sum, not from a web page). Nothing else in the archive is read or kept.
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { Readable } from 'node:stream';
import zlib from 'node:zlib';

// Dawn release v20261016.190333 (2026-10-17), commit 67d4d6e64b0755d638f76549794692badfdbc8ab
export const DAWN_TAG = 'v20261016.190333';
export const DAWN_COMMIT = '67d4d6e64b0755d638f76549794692badfdbc8ab';

/** Per platform: the release archive's flavour, the binary inside it, and the SHA-256 of that binary */
export const TINT_PINS = {
  'win32-x64': { flavour: 'windows-latest', entry: 'bin/tint.exe', file: 'tint.exe', sha256: '650ed934de155aeb190f95946f65738af84ccbac12544aaee9d7fe9ab4912661' },
  'linux-x64': { flavour: 'ubuntu-latest', entry: 'bin/tint', file: 'tint', sha256: '20afcd8fe818dce95585f1a584502fc4fcf38d5301f2fe7bb72038832137ff63' },
  'darwin-arm64': { flavour: 'macos-latest', entry: 'bin/tint', file: 'tint', sha256: 'aac31df95ad9cccfa3a9f20a98407f8a427a5ceece27166d680466fc3d952e3a' },
  'darwin-x64': { flavour: 'macos-15-intel', entry: 'bin/tint', file: 'tint', sha256: '8d85f86b115a943f78462c434344d8fc81432f6be378bdee672de96143486f31' },
};

export const tintKey = (platform = process.platform, arch = process.arch) => `${platform}-${arch}`;

export function archiveUrl(pin) {
  return `https://github.com/google/dawn/releases/download/${DAWN_TAG}/Dawn-${DAWN_COMMIT}-${pin.flavour}-Release.tar.gz`;
}

/** The folder n2build keeps downloaded tools in (beside n2build.json), or null when there is no home folder */
export function toolsFolder(env = process.env, platform = process.platform) {
  if (platform === 'win32') return env.APPDATA ? path.join(env.APPDATA, 'N2Engine', 'tools') : null;
  if (env.XDG_DATA_HOME && path.isAbsolute(env.XDG_DATA_HOME)) return path.join(env.XDG_DATA_HOME, 'n2engine', 'tools');
  const home = env.HOME || os.homedir();
  return home ? path.join(home, '.local', 'share', 'n2engine', 'tools') : null;
}

export const sha256Of = (data) => crypto.createHash('sha256').update(data).digest('hex');

/** The cached binary's path when it is there and is the pinned binary, else null */
export function cachedTint({ env = process.env, platform = process.platform, arch = process.arch } = {}) {
  const pin = TINT_PINS[tintKey(platform, arch)];
  const folder = toolsFolder(env, platform);
  if (!pin || !folder) return null;
  const file = path.join(folder, `tint-${DAWN_COMMIT.slice(0, 12)}`, pin.file);
  try {
    return sha256Of(fs.readFileSync(file)) === pin.sha256 ? file : null;
  } catch {
    return null;
  }
}

/**
 * Reads a gzip-compressed tar stream until the entry `wanted` ('<root>/bin/tint'; the root folder name is ignored) is complete,
 * and returns its bytes; the rest of the stream is not read. Returns null when the stream ends without it.
 * @param {AsyncIterable<Uint8Array>} chunks the compressed bytes
 */
export async function extractTarGzEntry(chunks, wanted) {
  const gunzip = zlib.createGunzip();
  const source = Readable.from(chunks);
  source.on('error', (error) => gunzip.destroy(error));
  source.pipe(gunzip);
  let pending = Buffer.alloc(0);
  let skip = 0; // bytes of the current entry's body (and its padding) still to drop
  let collect = null; // { size, parts, have, padding } while the wanted entry is being read
  let header = null;
  const take = (count) => { const out = pending.subarray(0, count); pending = pending.subarray(count); return out; };
  try {
    for await (const chunk of gunzip) {
      pending = pending.length === 0 ? Buffer.from(chunk) : Buffer.concat([pending, chunk]);
      for (;;) {
        if (collect) {
          const want = collect.size - collect.have;
          if (want > 0) {
            if (pending.length === 0) break;
            const part = take(Math.min(want, pending.length));
            collect.parts.push(Buffer.from(part));
            collect.have += part.length;
            continue;
          }
          return Buffer.concat(collect.parts);
        }
        if (skip > 0) {
          const dropped = Math.min(skip, pending.length);
          take(dropped);
          skip -= dropped;
          if (skip > 0) break;
          continue;
        }
        if (pending.length < 512) break;
        header = take(512);
        if (header.every((byte) => byte === 0)) continue; // the end-of-archive blocks
        const field = (start, length) => header.subarray(start, start + length).toString('latin1').replace(/\0.*$/s, '');
        let name = field(0, 100);
        const prefix = field(345, 155);
        if (prefix) name = `${prefix}/${name}`;
        const type = field(156, 1);
        const sizeText = field(124, 12).trim();
        const size = /^[0-7]+$/.test(sizeText) ? parseInt(sizeText, 8) : NaN;
        if (!Number.isFinite(size)) throw new Error(`the archive has an entry (${name}) with an unreadable size`);
        const bodyBlocks = Math.ceil(size / 512) * 512;
        if ((type === '0' || type === '') && name.split('/').slice(1).join('/') === wanted) {
          collect = { size, parts: [], have: 0 };
        } else if (type === 'x' || type === 'g' || type === 'L') {
          skip = bodyBlocks; // an extended header: not needed (the names read here are short)
        } else {
          skip = bodyBlocks;
        }
      }
    }
  } finally {
    source.destroy();
    gunzip.destroy();
  }
  return null;
}

async function* bodyChunks(response) {
  const reader = response.body.getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) return;
      yield value;
    }
  } finally {
    await reader.cancel().catch(() => {});
  }
}

/**
 * Downloads the pinned Tint into the tools folder and returns its path. Throws an Error that says what to do when it can't.
 * @param {object} [options]
 * @param {typeof fetch} [options.fetchImpl] injectable for tests
 * @param {(line: string) => void} [options.log]
 */
export async function installTint({ env = process.env, platform = process.platform, arch = process.arch, fetchImpl = fetch, log = () => {}, pins = TINT_PINS } = {}) {
  const key = tintKey(platform, arch);
  const pin = pins[key];
  const manual = 'Install Tint yourself and point the TINT environment variable at it (a build of Dawn: https://github.com/google/dawn/releases).';
  if (!pin) throw new Error(`There is no pinned Tint for ${key}. ${manual}`);
  const folder = toolsFolder(env, platform);
  if (!folder) throw new Error(`There is no folder to keep Tint in (no home folder). ${manual}`);
  const url = archiveUrl(pin);
  log(`Fetching Tint (the shader cook's WGSL compiler, ${key}) from Dawn ${DAWN_TAG}; only bin/${pin.file} is kept`);
  let response;
  try {
    response = await fetchImpl(url, { redirect: 'follow' });
  } catch (error) {
    throw new Error(`Tint could not be downloaded from ${url}: ${error.message}. ${manual}`);
  }
  if (!response.ok || !response.body) throw new Error(`Tint could not be downloaded from ${url} (HTTP ${response.status}). ${manual}`);
  let data;
  try {
    data = await extractTarGzEntry(bodyChunks(response), pin.entry);
  } catch (error) {
    throw new Error(`Tint's archive could not be read: ${error.message}. ${manual}`);
  }
  if (!data) throw new Error(`The Dawn archive has no ${pin.entry}. ${manual}`);
  const actual = sha256Of(data);
  if (actual !== pin.sha256) throw new Error(`The Tint binary's SHA-256 is ${actual}, not the pinned ${pin.sha256}: it was not kept. ${manual}`);
  const dir = path.join(folder, `tint-${DAWN_COMMIT.slice(0, 12)}`);
  fs.mkdirSync(dir, { recursive: true });
  const target = path.join(dir, pin.file);
  const temporary = `${target}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, data, { mode: 0o755 });
  fs.renameSync(temporary, target);
  fs.writeFileSync(path.join(dir, '.pin'), `dawn ${DAWN_TAG} ${DAWN_COMMIT} ${pin.sha256}\n`);
  log(`Tint kept in ${target}`);
  return target;
}
