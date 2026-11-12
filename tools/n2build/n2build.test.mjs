// Tests for tools/n2build. Run: node --test tools/n2build/n2build.test.mjs
// Nothing here compiles the engine: toolchains come from a fake probe, build trees are fake folders. The real builds run
// in CI (the n2build job in .github/workflows/tests.yml).
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import {
  archivePackage, buildKey, checkPhysics, needsConfigure, packageDesktop, packageWeb, planBuild, preparePackageDir,
  packPath, readProject, readTargetSettings, recordConfigure, resolveOptions, resolvePackFormat, runtimeFilesFor, safeName, writeMarker, PACKAGE_MARKER,
} from './lib/build.mjs';
import { loadDescriptors, readUserSettings, userSettingsFile, validateDescriptor } from './lib/descriptors.mjs';
import { BuildError, Reporter } from './lib/report.mjs';
import { MANIFEST_NAME, cookPack, crc32, findSevenZip, packFiles, writeZip } from './lib/cook.mjs';
import { resolveRequest } from './lib/serve.mjs';
import { applyEnv, checkToolchains, findOnPath, parseVersion, versionAtLeast } from './lib/toolchains.mjs';
import { ENGINE_ROOT, main, markerInfo, parseArguments, resolveConfig, UsageError } from './n2build.mjs';

const tmp = (prefix) => fs.mkdtempSync(path.join(os.tmpdir(), prefix));

function write(file, content) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, typeof content === 'string' ? content : JSON.stringify(content, null, 2));
}

function makeProject(project = { name: 'My Game!', projectId: '00000000-0000-0000-0000-000000000001' }, files = {}) {
  const dir = tmp('n2build-project-');
  write(path.join(dir, 'project.n2proj'), project);
  write(path.join(dir, 'assets', 'scene.lua'), '-- scene');
  for (const [relative, content] of Object.entries(files)) write(path.join(dir, relative), content);
  return dir;
}

function descriptor(overrides = {}) {
  return validateDescriptor({
    formatVersion: 1,
    name: 'fake',
    displayName: 'Fake',
    hostSystems: ['linux'],
    toolchains: { required: ['cmake'], optional: ['vulkan-sdk'] },
    physics3d: ['none', 'jolt'],
    cmake: {
      target: 'n2player',
      options: { N2ENGINE_BUILD_TESTS: 'OFF' },
      configs: { debug: { CMAKE_BUILD_TYPE: 'Debug' }, release: { CMAKE_BUILD_TYPE: 'Release' } },
      toolchainOptions: { 'vulkan-sdk': { present: { N2ENGINE_RENDERER_VULKAN: 'ON' }, absent: { N2ENGINE_RENDERER_VULKAN: 'OFF' } } },
    },
    package: { kind: 'desktop', from: 'bin' },
    run: { kind: 'executable' },
    ...overrides,
  }, 'fake/target.json');
}

/** A probe with the given files and commands: run(command) answers from `commands` by the command's base name */
function fakeProbe({ platform = 'linux', env = {}, files = [], commands = {}, dirs = {} } = {}) {
  const fileSet = new Set(files);
  return {
    platform,
    env: { PATH: platform === 'win32' ? 'C:\\tools' : '/usr/bin', ...env },
    exists: (file) => fileSet.has(file),
    readFile: (file) => (typeof commands[`read:${file}`] === 'string' ? commands[`read:${file}`] : null),
    listDir: (dir) => dirs[dir] ?? [],
    run(command, args) {
      // Either separator: a simulated Windows path must work when the tests run on Linux
      const name = command.split(/[\\/]/).pop().replace(/\.exe$/i, '');
      const answer = commands[name];
      if (answer === undefined) return null;
      if (typeof answer === 'function') return answer(args);
      return typeof answer === 'object' ? answer : { status: 0, stdout: answer, stderr: '' };
    },
  };
}

function capture() {
  const lines = [];
  const reporter = new Reporter({ json: true, out: (line) => lines.push(JSON.parse(line)), err: () => {} });
  return { reporter, lines };
}

// --- descriptors -------------------------------------------------------------------------------------------------

test('the repository\'s descriptors load and are valid', () => {
  const descriptors = loadDescriptors(ENGINE_ROOT);
  assert.deepEqual([...descriptors.keys()].sort(), ['linux', 'web', 'windows']);
  for (const d of descriptors.values()) {
    assert.equal(d.source, 'targets/');
    assert.ok(d.cmake.configs.debug && d.cmake.configs.release, `${d.name} has debug and release`);
  }
  assert.equal(descriptors.get('web').cmake.toolchain, 'emsdk');
  assert.equal(descriptors.get('windows').package.kind, 'desktop');
});

test('a descriptor with a mistake is refused with the file and the reason', () => {
  const base = JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'linux', 'target.json'), 'utf8'));
  const bad = [
    [{ formatVersion: 2 }, /formatVersion/],
    [{ name: 'Linux!' }, /name must be/],
    [{ hostSystems: [] }, /hostSystems/],
    [{ toolchains: { required: ['xcode-magic'] } }, /unknown toolchain 'xcode-magic'/],
    [{ physics3d: ['havok'] }, /physics3d: unknown value 'havok'/],
    [{ cmake: { ...base.cmake, configs: {} } }, /cmake.configs/],
    [{ cmake: { ...base.cmake, options: { 'not a var': 'x' } } }, /not a CMake variable name/],
    [{ cmake: { ...base.cmake, toolchainOptions: { emsdk: { present: {} } } } }, /not one of this target's toolchains/],
    [{ package: { kind: 'ipa', from: 'x' } }, /package.kind/],
    [{ run: { kind: 'teleport' } }, /run.kind/],
    [{ pins: { emsdk: 4 } }, /pins.emsdk/],
  ];
  for (const [change, message] of bad) {
    assert.throws(() => validateDescriptor({ ...base, ...change }, 'x/target.json'), (error) => error instanceof BuildError && message.test(error.message) && error.message.startsWith('x/target.json'), JSON.stringify(change));
  }
});

test('extra target folders add targets, and a name defined twice names both files', () => {
  const extra = tmp('n2build-targets-');
  const console = JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'linux', 'target.json'), 'utf8'));
  write(path.join(extra, 'handheld', 'target.json'), { ...console, name: 'handheld', displayName: 'A handheld' });
  const descriptors = loadDescriptors(ENGINE_ROOT, [extra]);
  assert.equal(descriptors.get('handheld').source, extra);

  write(path.join(extra, 'linux', 'target.json'), console);
  assert.throws(() => loadDescriptors(ENGINE_ROOT, [extra]), /defined twice: .*linux.*target\.json and .*linux.*target\.json/s);

  const mismatch = tmp('n2build-targets-');
  write(path.join(mismatch, 'other', 'target.json'), { ...console, name: 'notother' });
  assert.throws(() => loadDescriptors(ENGINE_ROOT, [mismatch]), /differs from its folder/);
  assert.throws(() => loadDescriptors(ENGINE_ROOT, [path.join(extra, 'missing')]), /does not exist/);
});

test('user settings: where they live, and a broken file is an error', () => {
  assert.equal(userSettingsFile({ APPDATA: 'C:\\Users\\a\\AppData\\Roaming' }, 'win32'), path.join('C:\\Users\\a\\AppData\\Roaming', 'N2Engine', 'n2build.json'));
  assert.equal(userSettingsFile({ HOME: '/home/a' }, 'linux'), path.join('/home/a', '.local', 'share', 'n2engine', 'n2build.json'));
  assert.equal(userSettingsFile({ XDG_DATA_HOME: '/data', HOME: '/home/a' }, 'linux'), path.join('/data', 'n2engine', 'n2build.json'));
  assert.deepEqual(readUserSettings(null), {});
  const dir = tmp('n2build-settings-');
  write(path.join(dir, 'ok.json'), { targetFolders: ['/private/targets'] });
  assert.deepEqual(readUserSettings(path.join(dir, 'ok.json')).targetFolders, ['/private/targets']);
  write(path.join(dir, 'bad.json'), '{ nope');
  assert.throws(() => readUserSettings(path.join(dir, 'bad.json')), /not valid JSON/);
  write(path.join(dir, 'wrong.json'), { targetFolders: 'one' });
  assert.throws(() => readUserSettings(path.join(dir, 'wrong.json')), /targetFolders/);
});

// --- project and physics -----------------------------------------------------------------------------------------

test('the physics check: a list asking for a backend the target cannot build fails with the reason', () => {
  const d = descriptor();
  assert.equal(checkPhysics(null, d), null, 'no list: every backend the target has');
  assert.equal(checkPhysics({ physics2d: true }, d), null, 'a list without physics3d keeps the default');
  assert.equal(checkPhysics({ physics3d: 'jolt' }, d), null);
  const problem = checkPhysics({ physics3d: 'physx' }, d);
  assert.match(problem, /physics3d is "physx", but the fake target can build only: none, jolt/);
});

test('readProject reads the module list and refuses folders that are not projects', () => {
  const project = readProject(makeProject({ name: 'G', build: { modules: { physics3d: 'none', physics2d: true } } }));
  assert.deepEqual(project.modules, { physics3d: 'none', physics2d: true });
  assert.throws(() => readProject(tmp('n2build-empty-')), /no project.n2proj/);
  assert.throws(() => readProject(makeProject({ name: 'G', build: { modules: { physics3d: 'havok' } } })), /physics3d/);
  const noAssets = tmp('n2build-noassets-');
  write(path.join(noAssets, 'project.n2proj'), { name: 'G' });
  assert.throws(() => readProject(noAssets), /no assets folder/);
});

test('safeName keeps a usable file name', () => {
  assert.equal(safeName('Triangle-Like'), 'Triangle-Like');
  assert.equal(safeName('My Game!'), 'My Game_');
  assert.equal(safeName('../evil'), '_evil');
  assert.equal(safeName(''), 'Game');
  assert.equal(safeName(undefined), 'Game');
});

// --- planning ----------------------------------------------------------------------------------------------------

test('the build tree is per target, configuration, module list, options and project', () => {
  const base = { projectDir: '/p', target: 'windows', config: 'release', modules: null, options: { A: '1' } };
  assert.equal(buildKey(base), buildKey({ ...base }));
  assert.match(buildKey(base), /^windows-release-[0-9a-f]{10}$/);
  for (const change of [{ config: 'debug' }, { modules: { physics3d: 'none' } }, { options: { A: '2' } }, { projectDir: '/q' }, { target: 'linux' }]) {
    assert.notEqual(buildKey({ ...base, ...change }), buildKey(base), JSON.stringify(change));
  }
});

test('options: configuration, then the optional toolchains present or absent', () => {
  const d = descriptor();
  assert.deepEqual(resolveOptions(d, 'release', new Set(['vulkan-sdk'])), { N2ENGINE_BUILD_TESTS: 'OFF', CMAKE_BUILD_TYPE: 'Release', N2ENGINE_RENDERER_VULKAN: 'ON' });
  assert.equal(resolveOptions(d, 'debug', new Set()).N2ENGINE_RENDERER_VULKAN, 'OFF');
  assert.throws(() => resolveOptions(d, 'profile', new Set()), /no 'profile' configuration .*debug, release/);
});

test('planBuild: the commands for a desktop and a web target', () => {
  const projectDir = makeProject({ name: 'Tri' });
  const descriptors = loadDescriptors(ENGINE_ROOT);
  const desktop = planBuild({ engineRoot: '/engine', projectDir, descriptor: descriptors.get('windows'), config: 'release', modules: null, present: new Set(), jobs: 4, gameName: 'Tri' });
  assert.ok(desktop.configureArgs.includes(`-DN2ENGINE_PROJECT=${path.resolve(projectDir)}`));
  assert.ok(desktop.configureArgs.includes('-DCMAKE_BUILD_TYPE=Release'));
  assert.ok(desktop.configureArgs.includes('-DN2ENGINE_RENDERER_VULKAN=OFF'));
  assert.ok(desktop.configureArgs.includes('-DN2ENGINE_TARGET=windows'), "the tree reads the target's own module list");
  assert.deepEqual(desktop.buildArgs.slice(2), ['--target', 'n2player', '-j', '4']);
  assert.equal(desktop.treeDir, path.join(projectDir, '.n2', 'build', desktop.key));
  assert.equal(desktop.packageDir, path.join(projectDir, '.n2', 'out', 'windows-release', 'Tri'));

  const web = planBuild({ engineRoot: '/engine', projectDir, descriptor: descriptors.get('web'), config: 'debug', modules: null, present: new Set(), toolchainFile: '/emsdk/Emscripten.cmake', gameName: 'Tri', outDir: path.join(projectDir, 'site') });
  assert.ok(web.configureArgs.includes('-DCMAKE_TOOLCHAIN_FILE=/emsdk/Emscripten.cmake'));
  assert.ok(web.configureArgs.includes('-DN2ENGINE_WEB_PROFILE=dev'));
  assert.ok(web.buildArgs.includes('web-package'));
  assert.equal(web.packageDir, path.join(projectDir, 'site'));
  assert.throws(() => planBuild({ engineRoot: '/e', projectDir, descriptor: descriptors.get('web'), config: 'release', modules: null, present: new Set(), toolchainFile: null, gameName: 'T' }), /toolchain file/);
});

test('a tree is configured again only when its arguments change', () => {
  const tree = tmp('n2build-tree-');
  assert.equal(needsConfigure(tree, ['a']), true, 'no cache');
  write(path.join(tree, 'CMakeCache.txt'), '');
  assert.equal(needsConfigure(tree, ['a']), true, 'no stamp');
  recordConfigure(tree, ['a']);
  assert.equal(needsConfigure(tree, ['a']), false);
  assert.equal(needsConfigure(tree, ['a', 'b']), true);
});

// --- toolchains --------------------------------------------------------------------------------------------------

test('versions', () => {
  assert.deepEqual(parseVersion('cmake version 3.30.1\n'), [3, 30, 1]);
  assert.deepEqual(parseVersion('g++ (GCC) 14.2'), [14, 2, 0]);
  assert.equal(parseVersion('nothing'), null);
  assert.ok(versionAtLeast([3, 25, 0], [3, 25, 0]));
  assert.ok(versionAtLeast([4, 0, 0], [3, 25, 0]));
  assert.ok(!versionAtLeast([3, 24, 9], [3, 25, 0]));
});

test('findOnPath honours PATHEXT on Windows', () => {
  const probe = fakeProbe({ platform: 'win32', env: { PATH: 'C:\\a;C:\\b', PATHEXT: '.EXE;.BAT' }, files: ['C:\\b\\cmake.EXE'.replace('.EXE', '.exe')] });
  assert.equal(findOnPath('cmake', probe.env, probe), 'C:\\b\\cmake.exe');
  assert.equal(findOnPath('ninja', probe.env, probe), null);
});

test('applyEnv: replace, set, then PATH prepended (Windows spells it Path)', () => {
  const env = applyEnv({ Path: 'C:\\x' }, { set: { A: '1' }, prependPath: ['C:\\y'], delimiter: ';' });
  assert.deepEqual(env, { Path: 'C:\\y;C:\\x', A: '1' });
  assert.deepEqual(applyEnv({ PATH: '/x' }, { replace: { PATH: '/v' } }), { PATH: '/v' });
});

test('doctor on Linux: cmake, ninja, the compiler version, and an optional Vulkan SDK', () => {
  const d = validateDescriptor({ ...JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'linux', 'target.json'), 'utf8')) }, 'linux');
  const files = ['/usr/bin/cmake', '/usr/bin/ninja', '/usr/bin/c++'];
  const ready = checkToolchains(d, fakeProbe({ files, commands: { cmake: 'cmake version 3.28.3', ninja: '1.11.1', 'c++': 'c++ (Ubuntu 14.2.0) 14.2.0' } }));
  assert.equal(ready.ok, true);
  assert.equal(ready.present.has('vulkan-sdk'), false);
  assert.equal(ready.checks.find((c) => c.id === 'vulkan-sdk').required, false);

  const oldGcc = checkToolchains(d, fakeProbe({ files, commands: { cmake: 'cmake version 3.28.3', ninja: '1.11.1', 'c++': 'c++ (Ubuntu 13.1.0) 13.1.0' } }));
  assert.equal(oldGcc.ok, false);
  assert.match(oldGcc.checks.find((c) => c.id === 'cxx').fix, /GCC 14/);

  const oldCmake = checkToolchains(d, fakeProbe({ files, commands: { cmake: 'cmake version 3.22.1', ninja: '1.11.1', 'c++': 'clang version 18.1.3' } }));
  assert.equal(oldCmake.ok, false);
  assert.match(oldCmake.checks.find((c) => c.id === 'cmake').detail, /older than 3\.25/);

  const noNinja = checkToolchains(d, fakeProbe({ files: ['/usr/bin/cmake', '/usr/bin/c++'], commands: { cmake: 'cmake version 3.28.3', 'c++': 'g++ 14.1.0' } }));
  assert.equal(noNinja.ok, false);
  assert.match(noNinja.checks.find((c) => c.id === 'ninja').fix, /ninja-build/);
});

test('doctor for the web: emsdk found, pinned version checked, environment and toolchain file set', () => {
  const d = loadDescriptors(ENGINE_ROOT).get('web');
  const emsdk = '/home/u/emsdk';
  const em = path.posix.join(emsdk, 'upstream', 'emscripten');
  const probe = (version) => fakeProbe({
    env: { HOME: '/home/u' },
    files: ['/usr/bin/cmake', '/usr/bin/ninja', path.posix.join(em, 'cmake', 'Modules', 'Platform', 'Emscripten.cmake')],
    dirs: { [path.posix.join(emsdk, 'node')]: ['22.16.0_64bit'] },
    commands: { cmake: 'cmake version 3.30.0', ninja: '1.12.0', [`read:${path.posix.join(em, 'emscripten-version.txt')}`]: `"${version}"\n` },
  });
  const ok = checkToolchains(d, probe('4.0.10'));
  assert.equal(ok.ok, true);
  assert.equal(ok.env.EMSDK, emsdk);
  assert.equal(ok.env.EMSDK_NODE, path.posix.join(emsdk, 'node', '22.16.0_64bit', 'bin', 'node'));
  assert.ok(ok.env.PATH.startsWith(`${emsdk}:${em}:`));
  assert.equal(ok.toolchainFile, path.posix.join(em, 'cmake', 'Modules', 'Platform', 'Emscripten.cmake'));

  const wrong = checkToolchains(d, probe('3.1.0'));
  assert.equal(wrong.ok, false);
  assert.match(wrong.checks.find((c) => c.id === 'emsdk').fix, /emsdk install 4\.0\.10/);

  const none = checkToolchains(d, fakeProbe({ env: { HOME: '/home/u' }, files: ['/usr/bin/cmake', '/usr/bin/ninja'], commands: { cmake: 'cmake version 3.30.0', ninja: '1.12' } }));
  assert.equal(none.ok, false);
  assert.match(none.checks.find((c) => c.id === 'emsdk').fix, /git clone https:\/\/github\.com\/emscripten-core\/emsdk/);
});

test('doctor on Windows: Visual Studio through vswhere and vcvars64, whose environment the build then uses', () => {
  const d = loadDescriptors(ENGINE_ROOT).get('windows');
  const vs = 'C:\\VS';
  const vswhere = 'C:\\PF86\\Microsoft Visual Studio\\Installer\\vswhere.exe';
  const vcvars = `${vs}\\VC\\Auxiliary\\Build\\vcvars64.bat`;
  const probe = fakeProbe({
    platform: 'win32',
    env: { 'ProgramFiles(x86)': 'C:\\PF86', PATH: 'C:\\Windows' },
    files: [vswhere, vcvars, 'C:\\MSVC\\bin\\cl.exe', 'C:\\CMake\\cmake.exe', 'C:\\CMake\\ninja.exe'],
    commands: {
      vswhere: `${vs}\r\n`,
      cmd: { status: 0, stdout: 'Path=C:\\MSVC\\bin;C:\\CMake;C:\\Windows\r\nVCToolsVersion=14.44.35207\r\nPATHEXT=.EXE;.BAT\r\n', stderr: '' },
      cmake: 'cmake version 3.30.1',
      ninja: '1.12.1',
    },
  });
  const result = checkToolchains(d, probe);
  assert.equal(result.ok, true, JSON.stringify(result.checks));
  assert.equal(result.checks.find((c) => c.id === 'msvc').version, '14.44.35207');
  assert.equal(result.env.Path, 'C:\\MSVC\\bin;C:\\CMake;C:\\Windows');

  const noVs = checkToolchains(d, fakeProbe({ platform: 'win32', env: { 'ProgramFiles(x86)': 'C:\\PF86' } }));
  assert.equal(noVs.ok, false);
  assert.match(noVs.checks.find((c) => c.id === 'msvc').fix, /Desktop development with C\+\+/);
});

// --- packaging ---------------------------------------------------------------------------------------------------

function fakeTree(files) {
  const tree = tmp('n2build-tree-');
  for (const [name, content] of Object.entries(files)) write(path.join(tree, 'bin', name), content);
  return tree;
}

test('a desktop package: the listed files, the player named after the game, and the game folder', () => {
  const projectDir = makeProject({ name: 'Tri' }, { '.import/a.meta': '{}' });
  const tree = fakeTree({ 'n2player-files.txt': 'n2player.exe\nOpenAL32.dll\n', 'n2player.exe': 'exe', 'OpenAL32.dll': 'dll', 'other.dll': 'not listed' });
  const packageDir = path.join(tmp('n2build-out-'), 'Tri');
  preparePackageDir(packageDir);
  const exe = packageDesktop({ treeDir: tree, descriptor: descriptor(), projectDir, packageDir, gameName: 'Tri', platform: 'win32' });
  assert.equal(exe, 'Tri.exe');
  assert.deepEqual(fs.readdirSync(packageDir).sort(), ['OpenAL32.dll', 'Tri.exe', 'game']);
  assert.ok(fs.existsSync(path.join(packageDir, 'game', 'project.n2proj')));
  assert.ok(fs.existsSync(path.join(packageDir, 'game', 'assets', 'scene.lua')));
  assert.ok(fs.existsSync(path.join(packageDir, 'game', '.import', 'a.meta')));
});

test('a desktop package refuses a list that names a path or a missing file', () => {
  const projectDir = makeProject();
  const out = tmp('n2build-out-');
  const escaping = fakeTree({ 'n2player-files.txt': 'n2player\n../secret\n', n2player: 'x' });
  assert.throws(() => packageDesktop({ treeDir: escaping, descriptor: descriptor(), projectDir, packageDir: out, gameName: 'G', platform: 'linux' }), /not a plain file name/);
  const missing = fakeTree({ 'n2player-files.txt': 'n2player\nlibopenal.so.1\n', n2player: 'x' });
  assert.throws(() => packageDesktop({ treeDir: missing, descriptor: descriptor(), projectDir, packageDir: out, gameName: 'G', platform: 'linux' }), /libopenal\.so\.1 is missing/);
  assert.throws(() => packageDesktop({ treeDir: tmp('n2build-tree-'), descriptor: descriptor(), projectDir, packageDir: out, gameName: 'G' }), /did not produce the player/);
});

test('a package folder is only ever emptied when n2build made it', () => {
  const parent = tmp('n2build-out-');
  const foreign = path.join(parent, 'mine');
  write(path.join(foreign, 'notes.txt'), 'keep me');
  assert.throws(() => preparePackageDir(foreign), /did not write/);
  assert.equal(fs.readFileSync(path.join(foreign, 'notes.txt'), 'utf8'), 'keep me');

  const ours = path.join(parent, 'ours');
  preparePackageDir(ours);
  write(path.join(ours, 'old.txt'), 'old');
  writeMarker(ours, { target: 'linux' });
  preparePackageDir(ours);
  assert.deepEqual(fs.readdirSync(ours), []);
  assert.ok(!fs.existsSync(path.join(ours, PACKAGE_MARKER)));
});

test('a web package copies the build\'s package folder', () => {
  const tree = tmp('n2build-tree-');
  for (const name of ['index.html', 'index.js', 'index.wasm', 'index.data', 'manifest.json']) write(path.join(tree, 'web-package', name), name);
  const out = tmp('n2build-out-');
  const web = loadDescriptors(ENGINE_ROOT).get('web');
  assert.equal(packageWeb({ treeDir: tree, descriptor: web, packageDir: out }), 'index.html');
  assert.deepEqual(fs.readdirSync(out).sort(), ['index.data', 'index.html', 'index.js', 'index.wasm', 'manifest.json']);
  assert.throws(() => packageWeb({ treeDir: tmp('n2build-tree-'), descriptor: web, packageDir: out }), /no index.html/);
});

const haveCmake = spawnSync('cmake', ['--version']).status === 0;
test('archives: .zip and .7z beside the package folder', { skip: !haveCmake && 'cmake is not on PATH' }, () => {
  const packageDir = path.join(tmp('n2build-out-'), 'Tri');
  write(path.join(packageDir, 'game', 'assets', 'scene.lua'), '-- scene');
  for (const format of ['zip', '7z']) {
    const archive = archivePackage({ packageDir, format });
    assert.equal(archive, `${packageDir}.${format}`);
    assert.ok(fs.statSync(archive).size > 0);
    const listing = spawnSync('cmake', ['-E', 'tar', 'tf', archive], { encoding: 'utf8' });
    assert.match(listing.stdout, /Tri\/game\/assets\/scene\.lua/);
  }
  assert.throws(() => archivePackage({ packageDir, format: 'rar' }), /zip or 7z/);
});

// --- the web server ----------------------------------------------------------------------------------------------

test('the web server never serves outside the package folder', () => {
  const root = path.resolve('/srv/game');
  assert.equal(resolveRequest(root, '/'), path.join(root, 'index.html'));
  assert.equal(resolveRequest(root, '/index.wasm?v=1'), path.join(root, 'index.wasm'));
  assert.equal(resolveRequest(root, '/../secret'), null);
  assert.equal(resolveRequest(root, '/%2e%2e/secret'), null);
  assert.equal(resolveRequest(root, '/a%00b'), null);
  assert.equal(resolveRequest(root, '/%E0%A4%A'), null);
});

// --- the command line --------------------------------------------------------------------------------------------

test('arguments', () => {
  const { command, options } = parseArguments(['build', '--target', 'web', '--project=game', '--archive', '7z', '--jobs', '8', '--json', '--targets-dir', 'a', '--targets-dir=b']);
  assert.equal(command, 'build');
  assert.equal(options.target, 'web');
  assert.equal(options.project, 'game');
  assert.equal(options.config, undefined, 'left to the project, then release (resolveConfig)');
  assert.equal(options.archive, '7z');
  assert.equal(options.jobs, 8);
  assert.equal(options.json, true);
  assert.deepEqual(options.targetsDir, ['a', 'b']);
  assert.equal(parseArguments([]).command, 'help');
  for (const bad of [['explode'], ['build', '--project', 'p'], ['build', '--target', 'web'], ['doctor', '--target'], ['build', '--target', 'w', '--project', 'p', '--archive', 'rar'], ['run', '--target', 'w', '--project', 'p', '--frames', '0'], ['list', '--json=yes'], ['list', '--colour'], ['build', '--target', 'w', '--project', 'p', '--config', 'Release; rm']]) {
    assert.throws(() => parseArguments(bad), UsageError, JSON.stringify(bad));
  }
});

test('list --json reports every target and whether it builds here', async () => {
  const { reporter, lines } = capture();
  const code = await main(['list', '--json'], { ctx: fakeProbe({ platform: 'linux', env: { HOME: tmp('n2build-home-') } }), reporter });
  assert.equal(code, 0);
  const targets = lines.find((l) => l.type === 'targets').targets;
  assert.deepEqual(targets.map((t) => [t.name, t.buildableHere]), [['linux', true], ['web', true], ['windows', false]]);
  assert.deepEqual(lines.at(-1), { type: 'result', ok: true });
});

test('build refuses a project whose physics the target cannot build, before compiling anything', async () => {
  const extra = tmp('n2build-targets-');
  const linux = JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'linux', 'target.json'), 'utf8'));
  write(path.join(extra, 'nophysx', 'target.json'), { ...linux, name: 'nophysx', displayName: 'No PhysX', physics3d: ['none', 'jolt'] });
  const projectDir = makeProject({ name: 'G', build: { modules: { physics3d: 'physx' } } });
  const { reporter, lines } = capture();
  const ctx = fakeProbe({ files: ['/usr/bin/cmake', '/usr/bin/ninja', '/usr/bin/c++'], env: { HOME: tmp('n2build-home-') }, commands: { cmake: 'cmake version 3.30.0', ninja: '1.12', 'c++': 'g++ 14.2.0' } });
  const code = await main(['build', '--target', 'nophysx', '--project', projectDir, '--targets-dir', extra, '--json'], { ctx, reporter });
  assert.equal(code, 1);
  const result = lines.at(-1);
  assert.equal(result.ok, false);
  assert.match(result.error, /physics3d is "physx", but the nophysx target can build only: none, jolt/);
  assert.ok(!fs.existsSync(path.join(projectDir, '.n2')), 'nothing was configured or built');
});

test('build --dry-run prints the plan without running anything', async () => {
  const projectDir = makeProject({ name: 'Tri' });
  const { reporter, lines } = capture();
  const ctx = fakeProbe({ files: ['/usr/bin/cmake', '/usr/bin/ninja', '/usr/bin/c++'], env: { HOME: tmp('n2build-home-') }, commands: { cmake: 'cmake version 3.30.0', ninja: '1.12', 'c++': 'g++ 14.2.0' } });
  const code = await main(['build', '--target', 'linux', '--project', projectDir, '--dry-run', '--json'], { ctx, reporter });
  assert.equal(code, 0);
  const logs = lines.filter((l) => l.type === 'log').map((l) => l.text);
  assert.ok(logs.some((t) => t.startsWith('cmake -S ') && t.includes('-DN2ENGINE_PROJECT=')), logs.join('\n'));
  assert.ok(logs.some((t) => t.includes('--target n2player')));
  const result = lines.at(-1);
  assert.equal(result.ok, true);
  assert.equal(result.dryRun, true);
  assert.ok(!fs.existsSync(path.join(projectDir, '.n2')));
});

test('doctor exits 1 and names the fix when a required tool is missing', async () => {
  const { reporter, lines } = capture();
  const code = await main(['doctor', '--target', 'linux', '--json'], { ctx: fakeProbe({ env: { HOME: tmp('n2build-home-') } }), reporter });
  assert.equal(code, 1);
  const report = lines.find((l) => l.type === 'doctor');
  assert.equal(report.ok, false);
  assert.ok(report.checks.find((c) => c.id === 'cmake').fix);
});

test('usage errors exit 2 with the usage', async () => {
  const { reporter, lines } = capture();
  assert.equal(await main(['build', '--json'], { ctx: fakeProbe(), reporter }), 2);
  assert.match(lines.at(-1).error, /needs --target/);
});

// --- build.targets in the project (build targets stage 3) ----------------------------------------------------------

test("a target's modules are laid over build.modules, key by key", () => {
  const projectDir = makeProject({
    name: 'G',
    build: {
      modules: { physics3d: 'both', physics2d: true, particles: false },
      targets: { android: { modules: { physics3d: 'jolt', particles: true } }, web: { enabled: true } },
    },
  });
  assert.deepEqual(readProject(projectDir, 'android').modules, { physics3d: 'jolt', physics2d: true, particles: true });
  assert.deepEqual(readProject(projectDir, 'web').modules, { physics3d: 'both', physics2d: true, particles: false });
  assert.deepEqual(readProject(projectDir).modules, { physics3d: 'both', physics2d: true, particles: false }, 'no target: build.modules');
  const onlyTarget = makeProject({ name: 'G', build: { targets: { linux: { modules: { physics3d: 'none' } } } } });
  assert.deepEqual(readProject(onlyTarget, 'linux').modules, { physics3d: 'none' });
  assert.equal(readProject(onlyTarget, 'web').modules, null, 'no list anywhere: every module');
  const bad = makeProject({ name: 'G', build: { targets: { linux: { modules: { physics3d: 'havok' } } } } });
  assert.throws(() => readProject(bad, 'linux'), /build\.targets\.linux\.modules\.physics3d must be one of/);
});

test("a target's physics list is what the physics check sees", () => {
  const nophysx = descriptor({ physics3d: ['none', 'jolt'] });
  const projectDir = makeProject({ name: 'G', build: { modules: { physics3d: 'physx' }, targets: { fake: { modules: { physics3d: 'jolt' } } } } });
  assert.equal(checkPhysics(readProject(projectDir, 'fake').modules, nophysx), null);
  assert.match(checkPhysics(readProject(projectDir).modules, nophysx), /physx/);
});

test('displayName names the game on that target; version and buildNumber go into the marker', () => {
  const projectDir = makeProject({ name: 'Tri', build: { targets: { linux: { displayName: 'Triangle-Like', version: '1.2.3', buildNumber: 42 } } } });
  const project = readProject(projectDir, 'linux');
  assert.equal(project.name, 'Triangle-Like');
  assert.equal(readProject(projectDir, 'web').name, 'Tri', 'other targets keep the project name');
  const marker = markerInfo({ descriptor: { name: 'linux' }, config: 'release', entry: 'Triangle-Like', project, engine: 'abc', builtAt: 'now' });
  assert.deepEqual(marker, { target: 'linux', config: 'release', entry: 'Triangle-Like', game: 'Triangle-Like', version: '1.2.3', buildNumber: 42, engine: 'abc', builtAt: 'now' });
  const plain = markerInfo({ descriptor: { name: 'web' }, config: 'debug', entry: 'index.html', project: readProject(projectDir, 'web'), engine: null, builtAt: 'now' });
  assert.ok(!('version' in plain) && !('buildNumber' in plain));
});

test("the configuration: --config, else the target's config, else release", () => {
  assert.equal(resolveConfig({ config: 'debug' }, { config: 'release' }), 'debug');
  assert.equal(resolveConfig({}, { config: 'debug' }), 'debug');
  assert.equal(resolveConfig({}, {}), 'release');
});

test("target settings n2build reads are checked by the editor's rules", () => {
  const settings = (s) => readTargetSettings({ build: { targets: { linux: s } } }, 'linux');
  assert.deepEqual(readTargetSettings({}, 'linux'), {});
  assert.deepEqual(settings({ displayName: 'G', unknownLater: { x: 1 } }), { displayName: 'G', unknownLater: { x: 1 } });
  assert.throws(() => settings({ displayName: '' }), /displayName/);
  assert.throws(() => settings({ displayName: 'a\nb' }), /displayName/);
  assert.throws(() => settings({ version: '1.2' }), /version/);
  assert.throws(() => settings({ buildNumber: 0 }), /buildNumber/);
  assert.throws(() => settings({ buildNumber: 2100000001 }), /buildNumber/);
  assert.throws(() => settings({ config: 'profile' }), /config/);
  assert.throws(() => settings({ enabled: 'yes' }), /enabled/);
  assert.throws(() => readTargetSettings({ build: { targets: [] } }, 'linux'), /build\.targets must be an object/);
});

test("build --dry-run uses the target's config and name, and warns when the target is turned off", async () => {
  const projectDir = makeProject({ name: 'Tri', build: { targets: { linux: { enabled: false, config: 'debug', displayName: 'Tri Linux' } } } });
  const { reporter, lines } = capture();
  const ctx = fakeProbe({ files: ['/usr/bin/cmake', '/usr/bin/ninja', '/usr/bin/c++'], env: { HOME: tmp('n2build-home-') }, commands: { cmake: 'cmake version 3.30.0', ninja: '1.12', 'c++': 'g++ 14.2.0' } });
  const code = await main(['build', '--target', 'linux', '--project', projectDir, '--dry-run', '--json'], { ctx, reporter });
  assert.equal(code, 0);
  assert.ok(lines.some((l) => l.type === 'warning' && /turned off/.test(JSON.stringify(l))), JSON.stringify(lines));
  const logs = lines.filter((l) => l.type === 'log').map((l) => l.text);
  assert.ok(logs.some((t) => t.includes('-DCMAKE_BUILD_TYPE=Debug')), logs.join('\n'));
  assert.ok(logs.some((t) => t.includes('-DN2ENGINE_TARGET=linux')));
  assert.equal(lines.at(-1).output, path.join(projectDir, '.n2', 'out', 'linux-debug', 'Tri Linux'));
});

// --- packs (build targets stage 4; docs/packs.html) ----------------------------------------------------------------

function packProject() {
  return makeProject({ name: 'Packed', projectId: '00000000-0000-0000-0000-000000000002', build: { modules: { physics3d: 'none' } } }, {
    'assets/scripts/main.lua': 'return { answer = 42 }\n'.repeat(50),
    'assets/sub/ünïcödé.txt': 'text\n',
    'assets/data.bin': Buffer.from(Array.from({ length: 1024 }, (_, i) => i % 256)),
    '.import/scripts/main.lua.meta': '{"uuid":"x"}',
    '.n2/asset-state.json': '{}',
    'editor/tool.lua': '-- an editor extension: not part of the game',
  });
}

/** Lists and extracts an archive with CMake's own archiver (libarchive), so the test does not trust our own reader */
function extract(archive) {
  const dir = tmp('n2build-extract-');
  const result = spawnSync('cmake', ['-E', 'tar', 'xf', archive], { cwd: dir, encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  return dir;
}

test('the pack holds project.n2proj, assets/ and .import/, nothing else, in name order', () => {
  const projectDir = packProject();
  assert.deepEqual(packFiles(projectDir).map((f) => f.name), [
    '.import/scripts/main.lua.meta', 'assets/data.bin', 'assets/scene.lua', 'assets/scripts/main.lua', 'assets/sub/ünïcödé.txt', 'project.n2proj',
  ]);
});

test('a zip pack: deflate where it saves bytes, CRCs, and the same bytes from the same project', { skip: !haveCmake && 'cmake is not on PATH' }, () => {
  const projectDir = packProject();
  const out = tmp('n2build-pack-');
  const manifest = cookPack({ projectDir, outFile: path.join(out, 'a.n2pack'), format: 'zip', target: 'linux', game: 'Packed' });
  cookPack({ projectDir, outFile: path.join(out, 'b.n2pack'), format: 'zip', target: 'linux', game: 'Packed' });
  assert.ok(fs.readFileSync(path.join(out, 'a.n2pack')).equals(fs.readFileSync(path.join(out, 'b.n2pack'))), 'reproducible');
  assert.equal(manifest.fileCount, 6);
  assert.equal(manifest.format, 'zip');
  const lua = manifest.files.find((f) => f.path === 'assets/scripts/main.lua');
  assert.equal(lua.size, 'return { answer = 42 }\n'.length * 50);
  assert.match(lua.sha256, /^[0-9a-f]{64}$/);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(out, 'a.n2pack.manifest.json'), 'utf8')), manifest);

  const extracted = extract(path.join(out, 'a.n2pack'));
  assert.equal(fs.readFileSync(path.join(extracted, 'assets', 'scripts', 'main.lua'), 'utf8'), 'return { answer = 42 }\n'.repeat(50));
  assert.equal(fs.readFileSync(path.join(extracted, 'assets', 'sub', 'ünïcödé.txt'), 'utf8'), 'text\n');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(extracted, MANIFEST_NAME), 'utf8')), manifest);
  assert.ok(!fs.existsSync(path.join(extracted, '.n2')) && !fs.existsSync(path.join(extracted, 'editor')));
  const packed = fs.statSync(path.join(out, 'a.n2pack')).size;
  assert.ok(packed < manifest.totalSize, `deflate made it smaller (${packed} < ${manifest.totalSize})`);

  // stored: no compression at all (the web's server compresses the download)
  cookPack({ projectDir, outFile: path.join(out, 'c.n2pack'), format: 'stored', target: 'web', game: 'Packed' });
  const stored = fs.readFileSync(path.join(out, 'c.n2pack'));
  assert.ok(stored.includes(Buffer.from('return { answer = 42 }\n'.repeat(50))), 'stored bytes are the file');
  assert.equal(crc32(Buffer.from('123456789')), 0xcbf43926, 'the standard CRC-32 check value');
});

test('the cook step refuses a project that uses a module its list leaves out', () => {
  const projectDir = makeProject({ name: 'G', build: { modules: { physics3d: 'none' } } }, { 'assets/a.lua': 'go:AddComponent("Rigidbody")' });
  assert.throws(() => cookPack({ projectDir, outFile: path.join(tmp('n2build-pack-'), 'g.n2pack'), format: 'zip', target: 'linux', game: 'G' }),
    /modules do not fit[\s\S]*Rigidbody/);
});

const sevenZip = findSevenZip();
test('a 7z pack is non-solid: one block per file', { skip: !sevenZip && '7-Zip is not installed' }, () => {
  const projectDir = packProject();
  const pack = path.join(tmp('n2build-pack-'), 'g.n2pack');
  cookPack({ projectDir, outFile: pack, format: '7z', target: 'android', game: 'Packed', sevenZip });
  const listing = spawnSync(sevenZip, ['l', '-slt', pack], { encoding: 'utf8' });
  assert.equal(listing.status, 0, listing.stderr);
  assert.match(listing.stdout, /Solid = -/);
  assert.match(listing.stdout, /Path = assets[\\/]scripts[\\/]main\.lua/);
});

test('the pack format: --pack, the target\'s setting, the descriptor, then zip for desktop and stored for the web', () => {
  const descriptors = loadDescriptors(ENGINE_ROOT);
  assert.equal(resolvePackFormat(descriptors.get('linux'), {}, undefined), 'zip');
  assert.equal(resolvePackFormat(descriptors.get('web'), {}, undefined), 'stored');
  assert.equal(resolvePackFormat(descriptors.get('linux'), { pack: '7z' }, undefined), '7z');
  assert.equal(resolvePackFormat(descriptors.get('linux'), { pack: '7z' }, 'folder'), 'folder');
  assert.throws(() => readTargetSettings({ build: { targets: { linux: { pack: 'rar' } } } }, 'linux'), /pack must be one of/);
  assert.throws(() => parseArguments(['build', '--target', 'linux', '--project', 'p', '--pack', 'rar']), UsageError);
  assert.throws(() => parseArguments(['cook', '--target', 'linux', '--project', 'p', '--pack', 'folder']), UsageError);
});

test('a desktop package carries game.n2pack beside the player; --pack folder keeps the folder', () => {
  const projectDir = packProject();
  const tree = fakeTree({ 'n2player-files.txt': 'n2player\n', n2player: 'exe' });
  const packaged = path.join(tmp('n2build-out-'), 'Packed');
  preparePackageDir(packaged);
  packageDesktop({ treeDir: tree, descriptor: descriptor(), projectDir, packageDir: packaged, gameName: 'Packed', platform: 'linux', pack: { format: 'zip' } });
  assert.deepEqual(fs.readdirSync(packaged).sort(), ['Packed', 'game.n2pack', 'game.n2pack.manifest.json']);
  const folder = path.join(tmp('n2build-out-'), 'Packed');
  preparePackageDir(folder);
  packageDesktop({ treeDir: tree, descriptor: descriptor(), projectDir, packageDir: folder, gameName: 'Packed', platform: 'linux', pack: { format: 'folder' } });
  assert.deepEqual(fs.readdirSync(folder).sort(), ['Packed', 'game']);
});

test('the web plan preloads the pack: N2ENGINE_WEB_PACK names it in the build tree', () => {
  const projectDir = makeProject({ name: 'Tri' });
  const web = loadDescriptors(ENGINE_ROOT).get('web');
  const plan = planBuild({ engineRoot: '/engine', projectDir, descriptor: web, config: 'release', modules: null, present: new Set(), toolchainFile: '/em.cmake', gameName: 'Tri', packFormat: 'stored' });
  assert.ok(plan.configureArgs.includes(`-DN2ENGINE_WEB_PACK=${path.join(plan.treeDir, 'game.n2pack')}`), plan.configureArgs.join(' '));
  assert.equal(packPath({ descriptor: web, packageDir: '/out', treeDir: plan.treeDir }), path.join(plan.treeDir, 'game.n2pack'));
  const folder = planBuild({ engineRoot: '/engine', projectDir, descriptor: web, config: 'release', modules: null, present: new Set(), toolchainFile: '/em.cmake', gameName: 'Tri', packFormat: 'folder' });
  assert.ok(!folder.configureArgs.some((a) => a.startsWith('-DN2ENGINE_WEB_PACK')));
});

// --- PhysX's GPU library only for GPU physics (follow-up of the build targets plan) ----------------------------------

test("PhysX's GPU library ships only when the project's physics.gpu is on or auto", () => {
  const runtime = ['OpenAL32.dll', 'PhysX_64.dll', 'PhysXCommon_64.dll', 'PhysXGpu_64.dll'];
  assert.deepEqual(runtimeFilesFor(runtime, undefined), { ship: ['OpenAL32.dll', 'PhysX_64.dll', 'PhysXCommon_64.dll'], left: ['PhysXGpu_64.dll'] });
  assert.deepEqual(runtimeFilesFor(runtime, 'off').left, ['PhysXGpu_64.dll']);
  assert.deepEqual(runtimeFilesFor(runtime, 'on'), { ship: runtime, left: [] });
  assert.deepEqual(runtimeFilesFor(runtime, 'auto').left, []);
  assert.deepEqual(runtimeFilesFor(['libopenal.so.1', 'libPhysXGpu_64.so'], 'off').left, ['libPhysXGpu_64.so']);
  assert.deepEqual(runtimeFilesFor(['PhysXGpu_64.dll.bak'], 'off').left, [], 'only the library itself');

  const tree = fakeTree({ 'n2player-files.txt': 'n2player.exe\nPhysX_64.dll\nPhysXGpu_64.dll\n', 'n2player.exe': 'exe', 'PhysX_64.dll': 'cpu', 'PhysXGpu_64.dll': 'gpu' });
  const left = [];
  const cpuOnly = path.join(tmp('n2build-out-'), 'Tri');
  preparePackageDir(cpuOnly);
  packageDesktop({ treeDir: tree, descriptor: descriptor(), projectDir: makeProject({ name: 'Tri' }), packageDir: cpuOnly, gameName: 'Tri', platform: 'win32',
    onLeftOut: (files) => left.push(...files) });
  assert.deepEqual(fs.readdirSync(cpuOnly).sort(), ['PhysX_64.dll', 'Tri.exe', 'game']);
  assert.deepEqual(left, ['PhysXGpu_64.dll']);
  const gpu = path.join(tmp('n2build-out-'), 'Tri');
  preparePackageDir(gpu);
  packageDesktop({ treeDir: tree, descriptor: descriptor(), projectDir: makeProject({ name: 'Tri' }), packageDir: gpu, gameName: 'Tri', platform: 'win32', physicsGpu: 'on' });
  assert.ok(fs.existsSync(path.join(gpu, 'PhysXGpu_64.dll')));
});
