// Building and packaging one project for one target. Plain steps, each testable on its own:
//   checkPhysics   the project's module list against what the target can build
//   planBuild      the build tree, the CMake configure and build commands, the package folder
//   runProcess     a tool, its output streamed to the reporter line by line
//   packageDesktop / packageWeb   the package folder from the build tree
//   archivePackage .zip or .7z of a package (CMake's own archiver, so no extra tool)
import { spawn, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { readModuleList } from '../../../scripts/project-modules.mjs';
import { PACK_EXTENSION, cookPack } from './cook.mjs';
import { PACK_KINDS } from './descriptors.mjs';
import { BuildError } from './report.mjs';

export const PACKAGE_MARKER = '.n2build-package.json';
export const ARCHIVE_FORMATS = { zip: 'zip', '7z': '7zip' };

/**
 * project.n2proj, parsed, with its module list checked as a build would check it. With a target, the list is that
 * target's (build.targets.<target>.modules over build.modules) and settings is build.targets.<target>, checked for the
 * keys n2build reads (displayName, version, buildNumber, config, enabled); name is the game's name on that target.
 */
export function readProject(projectDir, target) {
  const file = path.join(projectDir, 'project.n2proj');
  if (!fs.existsSync(file)) throw new BuildError(`${projectDir} is not a project folder: it has no project.n2proj`);
  let project;
  try {
    project = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    throw new BuildError(`${file}: not valid JSON (${error.message})`);
  }
  let modules;
  let settings;
  try {
    modules = readModuleList(project, target);
    settings = target ? readTargetSettings(project, target) : {};
  } catch (error) {
    throw new BuildError(`${file}: ${error.message}`);
  }
  if (!fs.existsSync(path.join(projectDir, 'assets'))) throw new BuildError(`${projectDir} has no assets folder`);
  const projectName = typeof project.name === 'string' ? project.name : '';
  return { file, project, modules, settings, name: settings.displayName ?? projectName };
}

const VERSION_PATTERN = /^\d{1,5}\.\d{1,5}\.\d{1,5}$/;
export const MAX_BUILD_NUMBER = 2100000000;

/**
 * build.targets.<target> (an empty object when absent), with the keys n2build acts on checked by the rules
 * IO::BuildTargets applies when the editor writes them (engine/include/engine/io/BuildTargets.hpp). Other keys are the
 * editor's and the later stages' and pass through unread. Throws an Error naming the key.
 */
export function readTargetSettings(project, target) {
  const targets = project?.build?.targets;
  if (targets === undefined) return {};
  if (!targets || typeof targets !== 'object' || Array.isArray(targets)) throw new Error('build.targets must be an object');
  const settings = targets[target];
  if (settings === undefined) return {};
  const where = `build.targets.${target}`;
  if (!settings || typeof settings !== 'object' || Array.isArray(settings)) throw new Error(`${where} must be an object`);
  if (settings.enabled !== undefined && typeof settings.enabled !== 'boolean') throw new Error(`${where}.enabled must be true or false`);
  if (settings.displayName !== undefined) {
    const name = settings.displayName;
    // eslint-disable-next-line no-control-regex
    if (typeof name !== 'string' || name.length < 1 || name.length > 64 || /[\u0000-\u001f\u007f]/.test(name)) {
      throw new Error(`${where}.displayName must be 1 to 64 characters with no control characters`);
    }
  }
  if (settings.version !== undefined && (typeof settings.version !== 'string' || !VERSION_PATTERN.test(settings.version))) {
    throw new Error(`${where}.version must be "major.minor.patch", each part 1 to 5 digits`);
  }
  if (settings.buildNumber !== undefined
    && !(Number.isInteger(settings.buildNumber) && settings.buildNumber >= 1 && settings.buildNumber <= MAX_BUILD_NUMBER)) {
    throw new Error(`${where}.buildNumber must be a whole number from 1 to ${MAX_BUILD_NUMBER}`);
  }
  if (settings.config !== undefined && !['debug', 'release'].includes(settings.config)) {
    throw new Error(`${where}.config must be "debug" or "release"`);
  }
  if (settings.pack !== undefined && !PACK_KINDS.includes(settings.pack)) {
    throw new Error(`${where}.pack must be one of ${PACK_KINDS.join(', ')}`);
  }
  return settings;
}

/**
 * How the game's files go into the package (docs/packs.html): --pack, else the target's build.targets pack, else the
 * descriptor's package.pack, else zip for a desktop package and stored (an uncompressed zip) for the web
 */
export function resolvePackFormat(descriptor, settings, optionPack) {
  return optionPack ?? settings?.pack ?? descriptor.package.pack ?? (descriptor.package.kind === 'web' ? 'stored' : 'zip');
}

/** Where the game's pack goes: beside the player in a desktop package, in the build tree for the web (index.data holds it) */
export function packPath({ descriptor, packageDir, treeDir }) {
  return descriptor.package.kind === 'web' ? path.join(treeDir, `game${PACK_EXTENSION}`) : path.join(packageDir, `game${PACK_EXTENSION}`);
}

/**
 * The physics module check per target: a project whose list asks for a 3D physics backend the target cannot build fails
 * here, with the reason, instead of at link time. A project without a list gets every backend the target has.
 * @returns {string|null} the problem, or null
 */
export function checkPhysics(modules, descriptor) {
  const wanted = modules?.physics3d;
  if (wanted === undefined) return null;
  if (descriptor.physics3d.includes(wanted)) return null;
  return `build.modules.physics3d is "${wanted}", but the ${descriptor.name} target can build only: ${descriptor.physics3d.join(', ')}. `
    + 'Change the list, or build for another target.';
}

/** A file and folder name made from the game's name: letters, digits, space, - _ . only */
export function safeName(name) {
  const cleaned = (name ?? '').replace(/[^A-Za-z0-9 _.-]/g, '_').replace(/^[ .]+|[ .]+$/g, '');
  return cleaned === '' ? 'Game' : cleaned.slice(0, 64);
}

/**
 * A build tree per target, configuration and module list (and project: the web packs the project into the build):
 * a script edit then rebuilds incrementally in the same tree, and switching lists never rebuilds everything.
 */
export function buildKey({ projectDir, target, config, modules, options }) {
  const hash = crypto.createHash('sha256')
    .update(JSON.stringify({ project: path.resolve(projectDir), modules: modules ?? null, options }))
    .digest('hex')
    .slice(0, 10);
  return `${target}-${config}-${hash}`;
}

/** The CMake options for a target, configuration and the toolchains present */
export function resolveOptions(descriptor, config, present) {
  const configOptions = descriptor.cmake.configs[config];
  if (!configOptions) {
    throw new BuildError(`The ${descriptor.name} target has no '${config}' configuration (it has: ${Object.keys(descriptor.cmake.configs).join(', ')})`);
  }
  const options = { ...descriptor.cmake.options, ...configOptions };
  for (const [id, entry] of Object.entries(descriptor.cmake.toolchainOptions)) {
    Object.assign(options, present.has(id) ? entry.present ?? {} : entry.absent ?? {});
  }
  return options;
}

/** Every path and command of a build, decided before anything runs (printed by --dry-run) */
export function planBuild({ engineRoot, projectDir, descriptor, config, modules, present, toolchainFile, buildRoot, outDir, jobs, gameName, packFormat }) {
  const options = resolveOptions(descriptor, config, present);
  // The pack format is not part of the key: the web's pack path is a configure argument (a change reconfigures the tree)
  const key = buildKey({ projectDir, target: descriptor.name, config, modules, options });
  const treeDir = path.join(buildRoot ?? path.join(projectDir, '.n2', 'build'), key);
  const packageDir = outDir ? path.resolve(outDir) : path.join(projectDir, '.n2', 'out', `${descriptor.name}-${config}`, safeName(gameName));
  // N2ENGINE_TARGET: cmake/N2EngineModules.cmake reads build.targets.<target>.modules over build.modules
  const configureArgs = ['-S', engineRoot, '-B', treeDir, '-G', descriptor.cmake.generator, `-DN2ENGINE_PROJECT=${path.resolve(projectDir)}`,
    `-DN2ENGINE_TARGET=${descriptor.name}`];
  // The web package preloads the game's pack instead of its folder (web/CMakeLists.txt, docs/packs.html)
  if (descriptor.package.kind === 'web' && packFormat !== undefined && packFormat !== 'folder') {
    configureArgs.push(`-DN2ENGINE_WEB_PACK=${packPath({ descriptor, packageDir: null, treeDir })}`);
  }
  if (descriptor.cmake.toolchain === 'emsdk') {
    if (!toolchainFile) throw new BuildError('The Emscripten toolchain file was not found (see n2build doctor)');
    configureArgs.push(`-DCMAKE_TOOLCHAIN_FILE=${toolchainFile}`);
  }
  for (const [name, value] of Object.entries(options).sort(([a], [b]) => a.localeCompare(b))) {
    configureArgs.push(`-D${name}=${value}`);
  }
  const buildArgs = ['--build', treeDir, '--target', descriptor.cmake.target];
  if (jobs) buildArgs.push('-j', String(jobs));
  return { key, treeDir, packageDir, options, configureArgs, buildArgs };
}

/** Whether the tree must be configured again: no cache yet, or different arguments from last time */
export function needsConfigure(treeDir, configureArgs) {
  const stamp = path.join(treeDir, 'n2build-configure.json');
  if (!fs.existsSync(path.join(treeDir, 'CMakeCache.txt')) || !fs.existsSync(stamp)) return true;
  try {
    return JSON.stringify(JSON.parse(fs.readFileSync(stamp, 'utf8'))) !== JSON.stringify(configureArgs);
  } catch {
    return true;
  }
}

export function recordConfigure(treeDir, configureArgs) {
  fs.writeFileSync(path.join(treeDir, 'n2build-configure.json'), `${JSON.stringify(configureArgs, null, 2)}\n`);
}

/** Runs a tool with its output streamed line by line to the reporter; resolves with the exit code */
export function runProcess(command, args, { env, cwd, reporter, spawnOptions = {} }) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { env, cwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], ...spawnOptions });
    const pipe = (stream, name) => {
      let pending = '';
      stream.setEncoding('utf8');
      stream.on('data', (chunk) => {
        pending += chunk;
        const lines = pending.split(/\r?\n/);
        pending = lines.pop();
        for (const line of lines) reporter.log(name, line);
      });
      stream.on('end', () => {
        if (pending !== '') reporter.log(name, pending);
      });
    };
    pipe(child.stdout, 'stdout');
    pipe(child.stderr, 'stderr');
    child.on('error', (error) => reject(new BuildError(`Could not start ${command}: ${error.message}`)));
    child.on('close', (code, signal) => resolve(signal ? 1 : code ?? 1));
  });
}

/**
 * Empties a package folder before it is written. Only a folder n2build made (it holds the marker), or an empty or
 * missing one: anything else is refused, so a mistyped --out never deletes someone's files.
 */
export function preparePackageDir(packageDir) {
  if (fs.existsSync(packageDir)) {
    const entries = fs.readdirSync(packageDir);
    if (entries.length > 0 && !entries.includes(PACKAGE_MARKER)) {
      throw new BuildError(`${packageDir} already holds files that n2build did not write; choose an empty or new folder with --out`);
    }
    fs.rmSync(packageDir, { recursive: true, force: true });
  }
  fs.mkdirSync(packageDir, { recursive: true });
}

function copyInto(from, to) {
  fs.cpSync(from, to, { recursive: true, dereference: true, errorOnExist: false, force: true });
}

/** The game's data beside the player: project.n2proj, assets/ and, when it has one, .import/ (the stored asset UUIDs) */
export function copyGameData(projectDir, gameDir) {
  fs.mkdirSync(gameDir, { recursive: true });
  copyInto(path.join(projectDir, 'project.n2proj'), path.join(gameDir, 'project.n2proj'));
  copyInto(path.join(projectDir, 'assets'), path.join(gameDir, 'assets'));
  if (fs.existsSync(path.join(projectDir, '.import'))) copyInto(path.join(projectDir, '.import'), path.join(gameDir, '.import'));
}

/** PhysX's GPU library (PhysXGpu_64.dll; libPhysXGpu_64.so on Linux): about 360 MB, and used only for GPU physics */
export const PHYSX_GPU_LIBRARY = /^(lib)?PhysXGpu_64\.(dll|so)$/i;

/**
 * The runtime files a package ships: every listed one, except PhysX's GPU library when the project's physics.gpu is off
 * (the default; "on" and "auto" use it). Returns {ship, left} (left: what was left out).
 */
export function runtimeFilesFor(files, physicsGpu) {
  const usesGpu = physicsGpu === 'on' || physicsGpu === 'auto';
  const left = usesGpu ? [] : files.filter((file) => PHYSX_GPU_LIBRARY.test(file));
  return { ship: files.filter((file) => !left.includes(file)), left };
}

/**
 * A desktop package: the player (renamed after the game) and the files players/desktop lists beside it, and the game
 * folder. Returns the executable's file name.
 */
export function packageDesktop({ treeDir, descriptor, projectDir, packageDir, gameName, platform = process.platform, pack = null, physicsGpu, onLeftOut }) {
  const binDir = path.join(treeDir, descriptor.package.from);
  const listFile = path.join(binDir, descriptor.package.fileList ?? 'n2player-files.txt');
  if (!fs.existsSync(listFile)) throw new BuildError(`${listFile} is missing: the build did not produce the player`);
  const files = fs.readFileSync(listFile, 'utf8').split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  const [player, ...runtime] = files;
  const extension = platform === 'win32' ? '.exe' : '';
  const executable = `${safeName(gameName)}${extension}`;
  for (const file of files) {
    if (file.includes('/') || file.includes('\\') || file === '..') throw new BuildError(`${listFile}: '${file}' is not a plain file name`);
    if (!fs.existsSync(path.join(binDir, file))) throw new BuildError(`${path.join(binDir, file)} is missing (listed in ${listFile})`);
  }
  fs.copyFileSync(path.join(binDir, player), path.join(packageDir, executable));
  if (platform !== 'win32') fs.chmodSync(path.join(packageDir, executable), 0o755);
  // PhysX's GPU library only for a game that uses GPU physics (physics.gpu on or auto): it is most of a Windows package
  const { ship, left } = runtimeFilesFor(runtime, physicsGpu);
  for (const file of ship) fs.copyFileSync(path.join(binDir, file), path.join(packageDir, file));
  if (left.length && onLeftOut) onLeftOut(left);
  // The game: one pack beside the player (it mounts game.n2pack where the game folder would be), or the folder as it is
  if (pack && pack.format !== 'folder') {
    cookPack({ projectDir, outFile: path.join(packageDir, `game${PACK_EXTENSION}`), format: pack.format, target: descriptor.name,
      game: gameName, sevenZip: pack.sevenZip, env: pack.env, skipModuleCheck: pack.skipModuleCheck });
  } else {
    copyGameData(projectDir, path.join(packageDir, 'game'));
  }
  return executable;
}

/** A web package: the folder the web-package target fills (the project is already packed into index.data) */
export function packageWeb({ treeDir, descriptor, packageDir }) {
  const from = path.join(treeDir, descriptor.package.from);
  if (!fs.existsSync(path.join(from, 'index.html'))) throw new BuildError(`${from} has no index.html: the build did not produce the web package`);
  for (const entry of fs.readdirSync(from)) copyInto(path.join(from, entry), path.join(packageDir, entry));
  return 'index.html';
}

export function writeMarker(packageDir, info) {
  fs.writeFileSync(path.join(packageDir, PACKAGE_MARKER), `${JSON.stringify(info, null, 2)}\n`);
}

export function readMarker(packageDir) {
  const file = path.join(packageDir, PACKAGE_MARKER);
  if (!fs.existsSync(file)) throw new BuildError(`${packageDir} is not a package n2build made (no ${PACKAGE_MARKER}); build it first`);
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

/** <package>.zip or .7z beside the package folder, holding the folder; returns its path */
export function archivePackage({ packageDir, format, cmake = 'cmake', env }) {
  const cmakeFormat = ARCHIVE_FORMATS[format];
  if (!cmakeFormat) throw new BuildError(`Unknown archive format '${format}' (zip or 7z)`);
  const parent = path.dirname(packageDir);
  const archive = path.join(parent, `${path.basename(packageDir)}.${format}`);
  fs.rmSync(archive, { force: true });
  const result = spawnSync(cmake, ['-E', 'tar', 'cf', archive, `--format=${cmakeFormat}`, path.basename(packageDir)], { cwd: parent, env, encoding: 'utf8', windowsHide: true });
  if (result.error || result.status !== 0) {
    throw new BuildError(`Could not write ${archive}: ${result.error?.message ?? result.stderr.trim()}`);
  }
  return archive;
}

/** The engine's commit, for the package marker; null outside a git checkout */
export function engineRevision(engineRoot) {
  const result = spawnSync('git', ['-C', engineRoot, 'rev-parse', 'HEAD'], { encoding: 'utf8', windowsHide: true });
  return result.status === 0 ? result.stdout.trim() : null;
}
