// Target descriptors: targets/<name>/target.json in the engine repository, plus folders the user adds (a private console
// target lives outside the repository). Data only: n2build has no code path named after a target.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { BuildError } from './report.mjs';

export const DESCRIPTOR_FORMAT = 1;
export const KNOWN_TOOLCHAINS = ['cmake', 'ninja', 'msvc', 'cxx', 'emsdk', 'vulkan-sdk', 'android-sdk', 'android-ndk', 'jdk', 'gradle'];
export const PACKAGE_KINDS = ['desktop', 'web', 'android'];
export const RUN_KINDS = ['executable', 'web-server', 'android-device'];
export const PHYSICS3D_VALUES = ['none', 'physx', 'jolt', 'both'];
const NAME = /^[a-z][a-z0-9-]{0,31}$/;
const CMAKE_VARIABLE = /^[A-Za-z_][A-Za-z0-9_]*$/;

/** Throws a BuildError naming the file and the first problem; returns the descriptor with defaults filled in */
export function validateDescriptor(raw, file) {
  const fail = (problem) => {
    throw new BuildError(`${file}: ${problem}`);
  };
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) fail('not a JSON object');
  if (raw.formatVersion !== DESCRIPTOR_FORMAT) fail(`formatVersion must be ${DESCRIPTOR_FORMAT}`);
  if (typeof raw.name !== 'string' || !NAME.test(raw.name)) fail('name must be lowercase letters, digits and -');
  if (typeof raw.displayName !== 'string' || raw.displayName === '') fail('displayName must be a non-empty string');
  const stringList = (value, what) => {
    if (!Array.isArray(value) || value.some((v) => typeof v !== 'string')) fail(`${what} must be a list of strings`);
    return value;
  };
  stringList(raw.hostSystems, 'hostSystems');
  if (raw.hostSystems.length === 0) fail('hostSystems must name at least one system (win32, linux, darwin)');

  const toolchains = raw.toolchains ?? {};
  const required = stringList(toolchains.required ?? [], 'toolchains.required');
  const optional = stringList(toolchains.optional ?? [], 'toolchains.optional');
  for (const id of [...required, ...optional]) {
    if (!KNOWN_TOOLCHAINS.includes(id)) fail(`unknown toolchain '${id}' (known: ${KNOWN_TOOLCHAINS.join(', ')})`);
  }

  if (raw.pins !== undefined) {
    if (raw.pins === null || typeof raw.pins !== 'object' || Array.isArray(raw.pins)) fail('pins must be an object');
    for (const [id, version] of Object.entries(raw.pins)) {
      if (!KNOWN_TOOLCHAINS.includes(id)) fail(`pins: unknown toolchain '${id}'`);
      if (typeof version !== 'string' || version === '') fail(`pins.${id} must be a version string`);
    }
  }
  if (raw.cmake?.toolchain !== undefined && raw.cmake.toolchain !== 'emsdk') fail("cmake.toolchain must be 'emsdk' when given");

  const physics3d = stringList(raw.physics3d ?? PHYSICS3D_VALUES, 'physics3d');
  for (const value of physics3d) {
    if (!PHYSICS3D_VALUES.includes(value)) fail(`physics3d: unknown value '${value}'`);
  }

  const cmake = raw.cmake;
  if (cmake === null || typeof cmake !== 'object') fail('cmake must be an object');
  if (typeof cmake.target !== 'string' || cmake.target === '') fail('cmake.target must name the CMake target to build');
  const optionMap = (value, what) => {
    if (value === undefined) return {};
    if (value === null || typeof value !== 'object' || Array.isArray(value)) fail(`${what} must be an object`);
    for (const [key, v] of Object.entries(value)) {
      if (!CMAKE_VARIABLE.test(key)) fail(`${what}: '${key}' is not a CMake variable name`);
      if (typeof v !== 'string') fail(`${what}.${key} must be a string`);
    }
    return value;
  };
  optionMap(cmake.options, 'cmake.options');
  if (cmake.configs === null || typeof cmake.configs !== 'object' || Object.keys(cmake.configs).length === 0) {
    fail('cmake.configs must map each configuration (debug, release) to its CMake options');
  }
  for (const [config, options] of Object.entries(cmake.configs)) optionMap(options, `cmake.configs.${config}`);
  const toolchainOptions = cmake.toolchainOptions ?? {};
  for (const [id, entry] of Object.entries(toolchainOptions)) {
    if (!optional.includes(id) && !required.includes(id)) fail(`cmake.toolchainOptions.${id}: not one of this target's toolchains`);
    optionMap(entry.present, `cmake.toolchainOptions.${id}.present`);
    optionMap(entry.absent, `cmake.toolchainOptions.${id}.absent`);
  }

  const pack = raw.package;
  if (pack === null || typeof pack !== 'object' || !PACKAGE_KINDS.includes(pack.kind)) {
    fail(`package.kind must be one of ${PACKAGE_KINDS.join(', ')}`);
  }
  if (typeof pack.from !== 'string' || pack.from === '') fail('package.from must name the folder in the build tree to package');
  const run = raw.run;
  if (run === null || typeof run !== 'object' || !RUN_KINDS.includes(run.kind)) fail(`run.kind must be one of ${RUN_KINDS.join(', ')}`);

  return {
    ...raw,
    toolchains: { required, optional },
    physics3d,
    cmake: { generator: 'Ninja', ...cmake, options: cmake.options ?? {}, toolchainOptions },
  };
}

/** Where the user's n2build settings live: beside the engine's user data (ProjectFile::UserDataBase) */
export function userSettingsFile(env = process.env, platform = process.platform) {
  if (platform === 'win32') {
    if (env.APPDATA) return path.join(env.APPDATA, 'N2Engine', 'n2build.json');
  } else {
    if (env.XDG_DATA_HOME && path.isAbsolute(env.XDG_DATA_HOME)) return path.join(env.XDG_DATA_HOME, 'n2engine', 'n2build.json');
    const home = env.HOME || os.homedir();
    if (home) return path.join(home, '.local', 'share', 'n2engine', 'n2build.json');
  }
  return null;
}

/** The user's settings ({} when there is no file); a broken file is an error, not silently ignored */
export function readUserSettings(file) {
  if (!file || !fs.existsSync(file)) return {};
  let settings;
  try {
    settings = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    throw new BuildError(`${file}: not valid JSON (${error.message})`);
  }
  if (settings === null || typeof settings !== 'object' || Array.isArray(settings)) throw new BuildError(`${file}: not a JSON object`);
  if (settings.targetFolders !== undefined && (!Array.isArray(settings.targetFolders) || settings.targetFolders.some((f) => typeof f !== 'string'))) {
    throw new BuildError(`${file}: targetFolders must be a list of folder paths`);
  }
  return settings;
}

/**
 * Every target: the repository's targets/ first, then each extra folder (each holding <name>/target.json). A name
 * defined twice is an error that names both files, so a private target can never silently replace a public one.
 * @returns {Map<string, object>} name -> descriptor (with .file and .source)
 */
export function loadDescriptors(engineRoot, extraFolders = []) {
  const found = new Map();
  const folders = [{ folder: path.join(engineRoot, 'targets'), source: 'targets/' }, ...extraFolders.map((folder) => ({ folder, source: folder }))];
  for (const { folder, source } of folders) {
    if (!fs.existsSync(folder)) {
      if (source !== 'targets/') throw new BuildError(`Target folder ${folder} does not exist`);
      continue;
    }
    for (const entry of fs.readdirSync(folder, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      if (!entry.isDirectory()) continue;
      const file = path.join(folder, entry.name, 'target.json');
      if (!fs.existsSync(file)) continue;
      let raw;
      try {
        raw = JSON.parse(fs.readFileSync(file, 'utf8'));
      } catch (error) {
        throw new BuildError(`${file}: not valid JSON (${error.message})`);
      }
      const descriptor = validateDescriptor(raw, file);
      if (descriptor.name !== entry.name) throw new BuildError(`${file}: name '${descriptor.name}' differs from its folder '${entry.name}'`);
      const previous = found.get(descriptor.name);
      if (previous) throw new BuildError(`Target '${descriptor.name}' is defined twice: ${previous.file} and ${file}`);
      found.set(descriptor.name, { ...descriptor, file, source, dir: path.dirname(file) });
    }
  }
  return found;
}

export function findTarget(descriptors, name) {
  const descriptor = descriptors.get(name);
  if (!descriptor) {
    throw new BuildError(`Unknown target '${name}'. Targets: ${[...descriptors.keys()].join(', ') || '(none)'}`);
  }
  return descriptor;
}
