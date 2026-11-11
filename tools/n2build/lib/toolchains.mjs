// Finding the tools a target needs, and the environment a build runs in. Every check reports what it found and, when
// something is missing, how to fix it. Nothing here installs anything.
//
// The probe (ctx) is injectable so the rules are unit tested without the tools: {platform, env, run, exists, readFile}.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const MIN_CMAKE = [3, 25, 0];
export const MIN_GCC = 14;

/** The real system */
export function systemProbe() {
  return {
    platform: process.platform,
    env: { ...process.env },
    run(command, args, options = {}) {
      const result = spawnSync(command, args, { encoding: 'utf8', windowsHide: true, timeout: 120000, ...options });
      if (result.error) return null;
      return { status: result.status, stdout: result.stdout ?? '', stderr: result.stderr ?? '' };
    },
    exists: (file) => fs.existsSync(file),
    readFile: (file) => {
      try {
        return fs.readFileSync(file, 'utf8');
      } catch {
        return null;
      }
    },
    listDir: (dir) => {
      try {
        return fs.readdirSync(dir);
      } catch {
        return [];
      }
    },
  };
}

/** The key PATH has in this environment (Windows spells it Path) */
export function pathKey(env) {
  return Object.keys(env).find((key) => key.toUpperCase() === 'PATH') ?? 'PATH';
}

/** A program on the PATH of env, with Windows' PATHEXT; null when absent */
export function findOnPath(name, env, ctx) {
  const delimiter = ctx.platform === 'win32' ? ';' : ':';
  const dirs = (env[pathKey(env)] ?? '').split(delimiter).filter(Boolean);
  const extensions = ctx.platform === 'win32' && !path.extname(name)
    ? (env.PATHEXT ?? '.EXE;.CMD;.BAT').split(';').filter(Boolean).map((e) => e.toLowerCase())
    : [''];
  const join = ctx.platform === 'win32' ? path.win32.join : path.posix.join;
  for (const dir of dirs) {
    for (const extension of extensions) {
      const candidate = join(dir.replace(/^"|"$/g, ''), name + extension);
      if (ctx.exists(candidate)) return candidate;
    }
  }
  return null;
}

/** "3.30.1" -> [3, 30, 1] */
export function parseVersion(text) {
  const match = /(\d+)\.(\d+)(?:\.(\d+))?/.exec(text ?? '');
  return match ? [Number(match[1]), Number(match[2]), Number(match[3] ?? 0)] : null;
}

export function versionAtLeast(version, minimum) {
  for (let i = 0; i < minimum.length; ++i) {
    if ((version[i] ?? 0) !== minimum[i]) return (version[i] ?? 0) > minimum[i];
  }
  return true;
}

/** Applies a check's environment change: {replace} (a whole environment, as vcvars gives) then {set} and {prependPath} */
export function applyEnv(env, change) {
  if (!change) return env;
  let next = change.replace ? { ...change.replace } : { ...env };
  for (const [key, value] of Object.entries(change.set ?? {})) next[key] = value;
  if (change.prependPath?.length) {
    const key = pathKey(next);
    const delimiter = change.delimiter ?? (process.platform === 'win32' ? ';' : ':');
    next[key] = [...change.prependPath, next[key] ?? ''].filter(Boolean).join(delimiter);
  }
  return next;
}

function vswhere(ctx, args) {
  const base = ctx.env['ProgramFiles(x86)'] ?? ctx.env.ProgramFiles ?? 'C:\\Program Files (x86)';
  const exe = path.win32.join(base, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe');
  if (!ctx.exists(exe)) return null;
  const result = ctx.run(exe, ['-latest', '-products', '*', ...args]);
  if (!result || result.status !== 0) return null;
  return result.stdout.trim().split(/\r?\n/)[0] || null;
}

/** Visual Studio's install folder with the x64 C++ tools, or null */
function visualStudio(ctx) {
  if (ctx._vs === undefined) {
    ctx._vs = vswhere(ctx, ['-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath']);
  }
  return ctx._vs;
}

const CHECKS = {
  msvc: {
    title: 'Visual Studio C++ (x64)',
    check(ctx) {
      if (ctx.platform !== 'win32') return { ok: false, detail: 'Windows only' };
      const install = visualStudio(ctx);
      const fix = 'Install Visual Studio 2022 (Community is enough) with the "Desktop development with C++" workload';
      if (!install) return { ok: false, detail: 'no Visual Studio with the x64 C++ tools was found (vswhere)', fix };
      const vcvars = path.win32.join(install, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat');
      if (!ctx.exists(vcvars)) return { ok: false, detail: `${vcvars} is missing`, fix };
      // The environment vcvars64 sets: run it and print the result (cmd's own quoting, so verbatim arguments)
      const result = ctx.run('cmd.exe', ['/d', '/s', '/c', `""${vcvars}" >nul && set"`], { windowsVerbatimArguments: true });
      if (!result || result.status !== 0) return { ok: false, detail: `${vcvars} failed`, fix };
      const env = {};
      for (const line of result.stdout.split(/\r?\n/)) {
        const at = line.indexOf('=');
        if (at > 0) env[line.slice(0, at)] = line.slice(at + 1);
      }
      if (!findOnPath('cl', env, ctx)) return { ok: false, detail: 'vcvars64 ran but cl.exe is not on its PATH', fix };
      const version = env.VCToolsVersion ?? env.VisualStudioVersion ?? null;
      return { ok: true, version, detail: install, env: { replace: env } };
    },
  },

  emsdk: {
    title: 'Emscripten SDK',
    check(ctx, pins) {
      const pinned = pins.emsdk;
      const fixFor = (dir) => `git clone https://github.com/emscripten-core/emsdk "${dir}", then in it: emsdk install ${pinned ?? 'latest'} and emsdk activate ${pinned ?? 'latest'} (or set EMSDK to an existing emsdk folder)`;
      const home = ctx.env.USERPROFILE ?? ctx.env.HOME ?? os.homedir();
      const join = ctx.platform === 'win32' ? path.win32.join : path.posix.join;
      const dir = ctx.env.EMSDK || join(home, 'emsdk');
      const emscripten = join(dir, 'upstream', 'emscripten');
      const versionText = ctx.readFile(join(emscripten, 'emscripten-version.txt'));
      if (versionText === null) return { ok: false, detail: `no emsdk with an activated Emscripten at ${dir}`, fix: fixFor(dir) };
      const version = versionText.trim().replace(/^"|"$/g, '');
      if (pinned && version !== pinned) {
        return { ok: false, version, detail: `Emscripten ${version} is active; this target is pinned to ${pinned}`, fix: `In ${dir}: emsdk install ${pinned} and emsdk activate ${pinned}` };
      }
      // emsdk's own node and (on Windows) python: newest folder of each
      const pick = (folder, test) => {
        const names = ctx.listDir(join(dir, folder)).filter(test).sort();
        return names.length ? join(dir, folder, names[names.length - 1]) : null;
      };
      const nodeDir = pick('node', (n) => /_64bit$/.test(n));
      const nodeBin = nodeDir ? (ctx.platform === 'win32' ? nodeDir : join(nodeDir, 'bin')) : null;
      const set = { EMSDK: dir, EM_CONFIG: join(dir, '.emscripten') };
      if (nodeBin) set.EMSDK_NODE = join(nodeBin, ctx.platform === 'win32' ? 'node.exe' : 'node');
      if (ctx.platform === 'win32') {
        const pythonDir = pick('python', (n) => /_64bit$/.test(n));
        if (pythonDir) set.EMSDK_PYTHON = join(pythonDir, 'python.exe');
      }
      const prependPath = [dir, emscripten, ...(nodeBin ? [nodeBin] : [])];
      const toolchainFile = join(emscripten, 'cmake', 'Modules', 'Platform', 'Emscripten.cmake');
      if (!ctx.exists(toolchainFile)) return { ok: false, version, detail: `${toolchainFile} is missing`, fix: fixFor(dir) };
      return { ok: true, version, detail: dir, env: { set, prependPath }, toolchainFile };
    },
  },

  cmake: {
    title: 'CMake',
    check(ctx, pins, env) {
      let exe = findOnPath('cmake', env, ctx);
      const extraPath = [];
      if (!exe && ctx.platform === 'win32' && visualStudio(ctx)) {
        // The CMake that comes with Visual Studio's C++ workload
        const bundled = path.win32.join(visualStudio(ctx), 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'CMake', 'bin');
        if (ctx.exists(path.win32.join(bundled, 'cmake.exe'))) {
          exe = path.win32.join(bundled, 'cmake.exe');
          extraPath.push(bundled);
        }
      }
      const fix = `Install CMake ${MIN_CMAKE.join('.')} or newer (https://cmake.org/download/ or your package manager)`;
      if (!exe) return { ok: false, detail: 'not found on PATH', fix };
      const result = ctx.run(exe, ['--version']);
      const version = parseVersion(result?.stdout);
      if (!version) return { ok: false, detail: `${exe} --version gave no version`, fix };
      if (!versionAtLeast(version, MIN_CMAKE)) return { ok: false, version: version.join('.'), detail: `older than ${MIN_CMAKE.join('.')}`, fix };
      return { ok: true, version: version.join('.'), detail: exe, env: extraPath.length ? { prependPath: extraPath } : undefined };
    },
  },

  ninja: {
    title: 'Ninja',
    check(ctx, pins, env) {
      let exe = findOnPath('ninja', env, ctx);
      const extraPath = [];
      if (!exe && ctx.platform === 'win32' && visualStudio(ctx)) {
        const bundled = path.win32.join(visualStudio(ctx), 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'Ninja');
        if (ctx.exists(path.win32.join(bundled, 'ninja.exe'))) {
          exe = path.win32.join(bundled, 'ninja.exe');
          extraPath.push(bundled);
        }
      }
      const fix = ctx.platform === 'win32'
        ? 'Install Visual Studio\'s C++ CMake tools (they include Ninja), or Ninja from https://ninja-build.org'
        : 'Install Ninja (apt install ninja-build, or your package manager)';
      if (!exe) return { ok: false, detail: 'not found on PATH', fix };
      const result = ctx.run(exe, ['--version']);
      return { ok: true, version: result?.stdout.trim() || null, detail: exe, env: extraPath.length ? { prependPath: extraPath } : undefined };
    },
  },

  cxx: {
    title: 'C++ compiler (C++23 with <generator>)',
    check(ctx, pins, env) {
      const fix = `Install GCC ${MIN_GCC} or newer (apt install g++-${MIN_GCC}) and set CXX=g++-${MIN_GCC} and CC=gcc-${MIN_GCC} if it is not the default`;
      const name = env.CXX || 'c++';
      const exe = path.isAbsolute(name) ? (ctx.exists(name) ? name : null) : findOnPath(name, env, ctx);
      if (!exe) return { ok: false, detail: `${name} not found`, fix };
      const result = ctx.run(exe, ['--version']);
      const text = result?.stdout ?? '';
      const version = parseVersion(text);
      if (!version) return { ok: false, detail: `${exe} --version gave no version`, fix };
      const clang = /clang/i.test(text);
      if (!clang && version[0] < MIN_GCC) {
        return { ok: false, version: version.join('.'), detail: `GCC ${version.join('.')} has no std::generator`, fix };
      }
      return { ok: true, version: `${clang ? 'clang' : 'gcc'} ${version.join('.')}`, detail: clang ? `${exe} (needs libstdc++ ${MIN_GCC} or newer for std::generator)` : exe };
    },
  },

  'vulkan-sdk': {
    title: 'Vulkan SDK (the Vulkan renderer)',
    check(ctx, pins, env) {
      const fix = 'Install the Vulkan SDK (https://vulkan.lunarg.com) or, on Linux, libvulkan-dev and glslang-tools. Without it the game is built with the OpenGL and software renderers only';
      if (env.VULKAN_SDK) {
        const header = ctx.platform === 'win32'
          ? path.win32.join(env.VULKAN_SDK, 'Include', 'vulkan', 'vulkan.h')
          : path.posix.join(env.VULKAN_SDK, 'include', 'vulkan', 'vulkan.h');
        if (ctx.exists(header)) return { ok: true, detail: env.VULKAN_SDK };
        return { ok: false, detail: `VULKAN_SDK=${env.VULKAN_SDK} has no vulkan.h`, fix };
      }
      if (ctx.platform !== 'win32' && ctx.exists('/usr/include/vulkan/vulkan.h')
          && (findOnPath('glslangValidator', env, ctx) || findOnPath('glslc', env, ctx))) {
        return { ok: true, detail: 'system headers and shader compiler' };
      }
      return { ok: false, detail: 'not found', fix };
    },
  },
};

// msvc and emsdk first: they change the environment the other checks look in
const ORDER = ['msvc', 'emsdk', 'cmake', 'ninja', 'cxx', 'vulkan-sdk'];

/**
 * Runs the checks a descriptor names, in dependency order.
 * @returns {{checks: object[], ok: boolean, env: object, toolchainFile: string|null, present: Set<string>}}
 */
export function checkToolchains(descriptor, ctx) {
  const wanted = new Map([
    ...descriptor.toolchains.required.map((id) => [id, true]),
    ...descriptor.toolchains.optional.map((id) => [id, false]),
  ]);
  let env = { ...ctx.env };
  let toolchainFile = null;
  const checks = [];
  const present = new Set();
  for (const id of ORDER) {
    if (!wanted.has(id)) continue;
    const required = wanted.get(id);
    const outcome = CHECKS[id].check(ctx, descriptor.pins ?? {}, env);
    if (outcome.ok) {
      present.add(id);
      env = applyEnv(env, outcome.env ? { ...outcome.env, delimiter: ctx.platform === 'win32' ? ';' : ':' } : null);
      if (outcome.toolchainFile) toolchainFile = outcome.toolchainFile;
    }
    checks.push({ id, title: CHECKS[id].title, required, ok: outcome.ok, version: outcome.version ?? null, detail: outcome.detail ?? '', fix: outcome.ok ? null : outcome.fix ?? null });
  }
  return { checks, ok: checks.every((c) => c.ok || !c.required), env, toolchainFile, present };
}
