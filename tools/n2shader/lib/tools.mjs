// Finding and running the external tools: glslang, spirv-val, spirv-dis, spirv-cross (Vulkan SDK or apt), Tint and naga (the
// pinned CI builds, scripts/install-wgsl-validators.sh). Every one is optional for the unit tests: a missing tool means its
// stages are skipped with a message, never a failure (cook --require-tools turns that around for CI).
import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

export const TOOL_NAMES = ['glslang', 'spirvVal', 'spirvDis', 'spirvCross', 'tint', 'naga'];

// environment variable and executable names per tool
const SPECS = {
  glslang: { env: 'GLSLANG', names: ['glslangValidator', 'glslang'] },
  spirvVal: { env: 'SPIRV_VAL', names: ['spirv-val'] },
  spirvDis: { env: 'SPIRV_DIS', names: ['spirv-dis'] },
  spirvCross: { env: 'SPIRV_CROSS', names: ['spirv-cross'] },
  tint: { env: 'TINT', names: ['tint'] },
  naga: { env: 'NAGA', names: ['naga'] },
};

function isFile(file) {
  try { return fs.statSync(file).isFile(); } catch { return false; }
}

function candidates(name, platform) {
  return platform === 'win32' ? [`${name}.exe`, `${name}.cmd`, name] : [name];
}

function sdkBinDirs(env, platform) {
  const dirs = [];
  if (env.VULKAN_SDK) dirs.push(path.join(env.VULKAN_SDK, platform === 'win32' ? 'Bin' : 'bin'), path.join(env.VULKAN_SDK, 'bin'));
  if (platform === 'win32') {
    try {
      const versions = fs.readdirSync('C:\\VulkanSDK').sort().reverse();
      for (const version of versions) dirs.push(path.join('C:\\VulkanSDK', version, 'Bin'));
    } catch { /* no SDK */ }
  }
  return dirs;
}

/** Resolves one tool: its environment variable, then PATH, then the Vulkan SDK. Returns an absolute path or null */
export function findTool(tool, { env = process.env, platform = process.platform } = {}) {
  const spec = SPECS[tool];
  if (!spec) throw new Error(`unknown tool ${tool}`);
  const given = env[spec.env];
  if (given) {
    if (isFile(given)) return path.resolve(given);
    const onPath = findOnPath(given, env, platform);
    return onPath;
  }
  for (const name of spec.names) {
    const found = findOnPath(name, env, platform);
    if (found) return found;
  }
  const dirs = tool === 'tint' || tool === 'naga' ? [] : sdkBinDirs(env, platform);
  for (const dir of dirs) {
    for (const name of spec.names) {
      for (const candidate of candidates(name, platform)) {
        if (isFile(path.join(dir, candidate))) return path.join(dir, candidate);
      }
    }
  }
  return null;
}

function findOnPath(name, env, platform) {
  if (path.isAbsolute(name)) return isFile(name) ? name : null;
  const separator = platform === 'win32' ? ';' : ':';
  for (const dir of (env.PATH ?? env.Path ?? '').split(separator).filter(Boolean)) {
    for (const candidate of candidates(name, platform)) {
      const full = path.join(dir, candidate);
      if (isFile(full)) return full;
    }
  }
  return null;
}

/** Locates every tool: { glslang: path|null, ... } */
export function discoverTools(options = {}) {
  return Object.fromEntries(TOOL_NAMES.map((tool) => [tool, findTool(tool, options)]));
}

/** Runs a tool, capturing its output. Never throws: a tool that cannot start is status -1 */
export function run(command, args, { input, cwd, timeoutMs = 60000 } = {}) {
  const result = spawnSync(command, args, { encoding: 'utf8', input, cwd, timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024, windowsHide: true });
  const status = result.status ?? (result.error ? -1 : 1);
  return { status, stdout: result.stdout ?? '', stderr: result.stderr ?? '', error: result.error?.message ?? null };
}

/** A version string good enough to record in a manifest and to tell two builds apart */
export function toolVersion(tool, file) {
  if (!file) return null;
  const firstLine = (text) => text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean)[0] ?? null;
  try {
    if (tool === 'glslang') {
      const out = run(file, ['--version']);
      const lines = out.stdout.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
      return lines.find((line) => /glslang/i.test(line)) ?? firstLine(out.stdout) ?? 'unknown';
    }
    if (tool === 'spirvVal' || tool === 'spirvDis') return firstLine(run(file, ['--version']).stdout) ?? 'unknown';
    if (tool === 'spirvCross') {
      const out = run(file, ['--revision']);
      return (out.stdout + out.stderr).split(/\r?\n/).map((line) => line.trim()).find((line) => /^Git commit/i.test(line)) ?? 'unknown';
    }
    if (tool === 'naga') {
      const out = run(file, ['--version']);
      return firstLine(out.stdout + out.stderr) ?? 'unknown';
    }
    if (tool === 'tint') {
      // Tint has no --version: the install script's pin stamp (<root>/.pin beside bin/), else the binary's hash
      const stamp = path.join(path.dirname(path.dirname(file)), '.pin');
      if (isFile(stamp)) return fs.readFileSync(stamp, 'utf8').trim();
      return `sha256:${crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').slice(0, 16)}`;
    }
  } catch { /* fall through */ }
  return 'unknown';
}

export const sha256 = (data) => crypto.createHash('sha256').update(data).digest('hex');
