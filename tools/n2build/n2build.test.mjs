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
import {
  androidSettings, cmakeInstallDir, defaultAppId, gradleInvocation, isAppId, packageAndroid, planAndroid, prepareGradleTree, propertiesPath,
} from './lib/android.mjs';
import { BuildError, Reporter } from './lib/report.mjs';
import { ACTIVITY, findAdb, parseDevices, pickDevice, runOnDevice } from './lib/adb.mjs';
import { ANDROID_SIGNING_ENV, androidSigningEnv, resolveAndroidSigning, signingProfileName, withoutSigningEnv } from './lib/signing.mjs';
import { MANIFEST_NAME, cookPack, crc32, findSevenZip, packFiles, writeZip } from './lib/cook.mjs';
import { resolveRequest } from './lib/serve.mjs';
import { applyEnv, checkToolchains, commandInvocation, findOnPath, parseVersion, versionAtLeast } from './lib/toolchains.mjs';
import { buildsRemotely, checkedToolchains, ENGINE_ROOT, main, markerInfo, parseArguments, resolveConfig, UsageError } from './n2build.mjs';
import { findAppBundle, iosConfigureArgs, iosSettings, packageIos, pickSimulator, runInSimulator } from './lib/apple.mjs';
import { checkPushed, dispatchAndFollow, gitState, pickRun, workflowInputs, workflowTemplate } from './lib/remote.mjs';

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
  assert.deepEqual([...descriptors.keys()].sort(), ['android', 'ios', 'linux', 'macos', 'web', 'windows']);
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
  assert.deepEqual(targets.map((t) => [t.name, t.buildableHere]), [['android', true], ['ios', true], ['linux', true], ['macos', true], ['web', true], ['windows', false]]);
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

// --- Android (build targets stage 8) -------------------------------------------------------------------------------

/** A Linux machine with everything the android target needs; overrides change one piece */
function androidProbe({ env = {}, files = [], commands = {}, dirs = {}, platform = 'linux' } = {}) {
  return fakeProbe({
    platform,
    env: { HOME: '/home/u', ANDROID_HOME: '/sdk', JAVA_HOME: '/jdk', ...env },
    files: ['/sdk/platform-tools', '/jdk/bin/java', '/usr/bin/gradle', '/usr/bin/cmake', '/usr/bin/ninja', ...files],
    dirs: { '/sdk/ndk': ['26.3.11579264', '27.0.12077973'], ...dirs },
    commands: {
      java: { status: 0, stdout: '', stderr: 'openjdk version "21.0.4" 2024-07-16' },
      gradle: 'Gradle 8.10.2',
      cmake: 'cmake version 3.30.0',
      ninja: '1.12.1',
      ...commands,
    },
  });
}

function androidDescriptor() {
  return loadDescriptors(ENGINE_ROOT).get('android');
}

test('the android descriptor: Jolt or no 3D physics, a pinned NDK, an APK from players/android', () => {
  const d = androidDescriptor();
  assert.equal(d.package.kind, 'android');
  assert.equal(d.package.from, 'players/android');
  assert.equal(d.package.pack, 'zip');
  assert.equal(d.run.kind, 'android-device');
  assert.deepEqual(d.physics3d, ['none', 'jolt']);
  assert.match(d.pins['android-ndk'], /^\d+\.\d+\.\d+$/);
  assert.ok(fs.existsSync(path.join(ENGINE_ROOT, d.package.from, 'app', 'build.gradle')));
  assert.match(checkPhysics({ physics3d: 'physx' }, d), /can build only: none, jolt/);
});

test('doctor for android: the SDK, the pinned NDK, a JDK 17 to 21 and Gradle 8.9 or newer', () => {
  const ok = checkToolchains(androidDescriptor(), androidProbe());
  assert.equal(ok.ok, true, JSON.stringify(ok.checks, null, 1));
  assert.equal(ok.info.androidSdk, '/sdk');
  assert.equal(ok.info.androidNdk, '27.0.12077973');
  assert.equal(ok.info.gradle, '/usr/bin/gradle');
  assert.equal(ok.env.JAVA_HOME, '/jdk');
  assert.equal(ok.env.ANDROID_HOME, '/sdk');

  const check = (probe, id) => checkToolchains(androidDescriptor(), probe).checks.find((c) => c.id === id);
  assert.match(check(androidProbe({ dirs: { '/sdk/ndk': ['28.2.13676358'] } }), 'android-ndk').detail, /27\.0\.12077973 is not installed \(found: 28/);
  assert.match(check(androidProbe({ env: { ANDROID_HOME: '/nowhere' } }), 'android-sdk').fix, /ANDROID_HOME/);
  const java26 = check(androidProbe({ commands: { java: { status: 0, stdout: '', stderr: 'openjdk version "26" 2026-03-17' } } }), 'jdk');
  assert.equal(java26.ok, false);
  assert.match(java26.detail, /\/jdk \(26\)/);
  assert.equal(check(androidProbe({ commands: { java: { status: 0, stdout: '', stderr: 'java version "1.8.0_402"' } } }), 'jdk').ok, false);
  const oldGradle = check(androidProbe({ commands: { gradle: 'Gradle 7.4' } }), 'gradle');
  assert.equal(oldGradle.ok, false);
  assert.match(oldGradle.detail, /older than 8\.9/);
});

test('doctor on Windows runs gradle.bat through cmd.exe, never with a shell string it did not check', () => {
  const seen = [];
  const probe = fakeProbe({
    platform: 'win32',
    env: { PATH: 'C:\\Gradle\\bin', PATHEXT: '.EXE;.BAT' },
    files: ['C:\\Gradle\\bin\\gradle.bat'],
    commands: { cmd: (args) => { seen.push(args); return { status: 0, stdout: 'Gradle 8.11.1', stderr: '' }; } },
  });
  const gradle = checkToolchains(validateDescriptor({ ...JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'android', 'target.json'), 'utf8')),
    toolchains: { required: ['gradle'] } }, 'android/target.json'), probe).checks[0];
  assert.equal(gradle.ok, true, gradle.detail);
  assert.equal(gradle.version, '8.11.1');
  assert.deepEqual(seen[0], ['/d', '/s', '/c', '"C:\\Gradle\\bin\\gradle.bat --version"']);
});

test('commandInvocation: a .bat goes through cmd.exe quoted; a character cmd.exe would read is refused', () => {
  assert.deepEqual(commandInvocation('/usr/bin/gradle', ['a b'], 'linux'), { command: '/usr/bin/gradle', args: ['a b'], options: {} });
  assert.deepEqual(commandInvocation('C:\\x\\gradle.exe', ['a'], 'win32').command, 'C:\\x\\gradle.exe');
  const bat = commandInvocation('C:\\Program Files\\Gradle\\bin\\gradle.bat', ['-p', 'C:\\My Games\\tree', '-Pn2.version=1.0.0'], 'win32');
  assert.equal(bat.command, 'cmd.exe');
  assert.deepEqual(bat.args, ['/d', '/s', '/c', '""C:\\Program Files\\Gradle\\bin\\gradle.bat" -p "C:\\My Games\\tree" -Pn2.version=1.0.0"']);
  assert.equal(bat.options.windowsVerbatimArguments, true);
  for (const bad of ['100%', 'a&b', 'a|b', 'a"b', 'x^y', 'a<b', 'a>b', 'wow!', 'two\nlines']) {
    assert.equal(commandInvocation('C:\\g\\gradle.bat', [bad], 'win32'), null, bad);
  }
  assert.throws(() => gradleInvocation('C:\\g\\gradle.bat', ['-Pn2.displayName=Rock & Roll'], 'win32'), BuildError);
});

test('app ids: the editor\'s rule plus no Java keyword; a default from the game\'s name', () => {
  for (const good of ['com.example.game', 'io.n2.Tri_Like2', 'a.b']) assert.ok(isAppId(good), good);
  for (const bad of ['game', 'com..game', 'com.1game', 'com.example.class', 'com.ex-ample.game', '.com.game', 'com.game.', `a.${'b'.repeat(150)}`, 42]) {
    assert.ok(!isAppId(bad), String(bad));
  }
  assert.equal(defaultAppId('Triangle-Like!'), 'com.n2engine.trianglelike');
  assert.equal(defaultAppId('2048'), 'com.n2engine.game2048');
  assert.equal(defaultAppId(''), 'com.n2engine.game');
  assert.equal(defaultAppId('Class'), 'com.n2engine.class_');
  assert.ok(isAppId(defaultAppId('Class')));
});

test('build.targets.android: appId, orientation and abis are checked', () => {
  assert.deepEqual(androidSettings({}, 'Tri'), { appId: 'com.n2engine.tri', orientation: 'any', abis: ['arm64-v8a', 'x86_64'], vulkan: false });
  assert.deepEqual(androidSettings({ appId: 'com.example.tri', orientation: 'landscape', abis: ['arm64-v8a'] }, 'Tri'),
    { appId: 'com.example.tri', orientation: 'landscape', abis: ['arm64-v8a'], vulkan: false });
  // The renderer (stage 12): vulkan builds the Vulkan renderer in; software and auto do not; desktop-only ones are refused
  assert.equal(androidSettings({ renderer: 'vulkan' }, 'Tri').vulkan, true);
  assert.equal(androidSettings({ renderer: 'auto' }, 'Tri').vulkan, false);
  assert.throws(() => androidSettings({ renderer: 'opengl' }, 'Tri'), /renderer must be "software" or "vulkan"/);
  assert.throws(() => androidSettings({ appId: 'tri' }, 'Tri'), /appId/);
  assert.throws(() => androidSettings({ orientation: 'sideways' }, 'Tri'), /orientation/);
  assert.throws(() => androidSettings({ abis: ['mips'] }, 'Tri'), /abis/);
  assert.throws(() => androidSettings({ abis: [] }, 'Tri'), /abis/);
  assert.throws(() => androidSettings({ abis: ['x86_64', 'x86_64'] }, 'Tri'), /abis/);
});

test('the android plan: cook into the tree, Gradle properties from the project, the APK path per configuration', () => {
  const projectDir = makeProject({ name: 'Tri Like' });
  const buildRoot = tmp('n2build-android-');
  const base = { engineRoot: ENGINE_ROOT, projectDir, descriptor: androidDescriptor(), modules: undefined, buildRoot, gameName: 'Tri Like',
    info: { androidSdk: 'C:\\Users\\me\\Android\\Sdk', androidNdk: '27.0.12077973' }, cmakeDir: 'C:\\Program Files\\CMake' };
  const release = planAndroid({ ...base, config: 'release', settings: { appId: 'com.example.tri', version: '1.2.3', buildNumber: 7, orientation: 'landscape' } });
  assert.equal(release.packFile, path.join(release.treeDir, 'assets', 'game.n2pack'));
  assert.equal(release.gradleDir, path.join(release.treeDir, 'gradle'));
  assert.ok(release.treeDir.startsWith(buildRoot));
  assert.equal(release.gradleArgs[0], '-p');
  assert.ok(release.gradleArgs.includes('assembleRelease'));
  assert.ok(release.gradleArgs.includes('--no-daemon'));
  for (const property of ['-Pn2.appId=com.example.tri', '-Pn2.version=1.2.3', '-Pn2.buildNumber=7', '-Pn2.orientation=sensorLandscape',
    '-Pn2.abis=arm64-v8a,x86_64', '-Pn2.ndkVersion=27.0.12077973', '-Pn2.cmakeBuildType=Release', '-Pn2.displayName=Tri Like',
    `-Pn2.project=${path.resolve(projectDir)}`, `-Pn2.assetsDir=${release.assetsDir}`]) {
    assert.ok(release.gradleArgs.includes(property), `${property} in ${release.gradleArgs.join(' ')}`);
  }
  assert.equal(release.apk, path.join(release.gradleDir, 'app', 'build', 'outputs', 'apk', 'release', 'app-release-unsigned.apk'));
  assert.equal(release.signed, false);
  assert.equal(release.localProperties, '# Written by n2build for this build tree\nsdk.dir=C\\:/Users/me/Android/Sdk\ncmake.dir=C\\:/Program Files/CMake\n');

  const debug = planAndroid({ ...base, config: 'debug', settings: {} });
  assert.ok(debug.gradleArgs.includes('assembleDebug'));
  assert.ok(debug.gradleArgs.includes('-Pn2.appId=com.n2engine.trilike'));
  assert.ok(debug.gradleArgs.includes('-Pn2.orientation=fullUser'));
  assert.equal(debug.signed, true);
  assert.notEqual(debug.treeDir, release.treeDir);
  assert.notEqual(planAndroid({ ...base, config: 'debug', settings: { abis: ['x86_64'] } }).treeDir, debug.treeDir, 'the ABIs are part of the tree');

  assert.throws(() => planAndroid({ ...base, config: 'profile', settings: {} }), /debug or release/);
  assert.throws(() => planAndroid({ ...base, config: 'debug', settings: { displayName: '100% Fun' } }), /n2\.displayName.*cannot pass/);
  assert.equal(propertiesPath('/opt/android-sdk'), '/opt/android-sdk');
  assert.equal(cmakeInstallDir(path.join('opt', 'cmake', 'bin', 'cmake')), path.join('opt', 'cmake'));
});

test('the Gradle tree: the template copied over (its build output and local.properties left out), local.properties written', () => {
  const template = tmp('n2build-template-');
  write(path.join(template, 'settings.gradle'), 'rootProject.name = "N2Player"');
  write(path.join(template, 'app', 'build.gradle'), 'android {}');
  write(path.join(template, 'app', 'build', 'stale.apk'), 'x');
  write(path.join(template, 'native', 'CMakeLists.txt'), 'native is built from the engine root');
  write(path.join(template, 'local.properties'), 'sdk.dir=/someone/elses');
  const treeDir = tmp('n2build-tree-');
  const plan = { template, gradleDir: path.join(treeDir, 'gradle'), assetsDir: path.join(treeDir, 'assets'), localProperties: 'sdk.dir=/sdk\n' };
  write(path.join(plan.gradleDir, 'app', 'build', 'intermediates', 'kept.txt'), 'incremental');
  prepareGradleTree(plan);
  assert.equal(fs.readFileSync(path.join(plan.gradleDir, 'app', 'build.gradle'), 'utf8'), 'android {}');
  assert.equal(fs.readFileSync(path.join(plan.gradleDir, 'local.properties'), 'utf8'), 'sdk.dir=/sdk\n');
  assert.ok(!fs.existsSync(path.join(plan.gradleDir, 'app', 'build', 'stale.apk')));
  assert.ok(!fs.existsSync(path.join(plan.gradleDir, 'native')));
  assert.ok(fs.existsSync(path.join(plan.gradleDir, 'app', 'build', 'intermediates', 'kept.txt')), 'Gradle\'s output stays for the next build');
  assert.ok(fs.statSync(plan.assetsDir).isDirectory());
});

test('the android package: the APK named after the game, -unsigned when it is not signed', () => {
  const dir = tmp('n2build-apk-');
  const apk = path.join(dir, 'app-release-unsigned.apk');
  write(apk, 'PK');
  const packageDir = path.join(dir, 'out');
  fs.mkdirSync(packageDir);
  assert.deepEqual(packageAndroid({ plan: { apk, signed: false }, packageDir, gameName: 'Tri Like!' }), { apk: 'Tri Like_-unsigned.apk', aab: null });
  assert.deepEqual(packageAndroid({ plan: { apk, signed: true }, packageDir, gameName: 'Tri' }), { apk: 'Tri.apk', aab: null });
  const aab = path.join(dir, 'app-release.aab');
  write(aab, 'PK');
  assert.deepEqual(packageAndroid({ plan: { apk, aab, signed: true }, packageDir, gameName: 'Tri' }), { apk: 'Tri.apk', aab: 'Tri.aab' });
  assert.ok(fs.existsSync(path.join(packageDir, 'Tri.aab')));
  assert.throws(() => packageAndroid({ plan: { apk, aab: path.join(dir, 'none.aab'), signed: true }, packageDir, gameName: 'Tri' }), /did not produce the AAB/);
  assert.throws(() => packageAndroid({ plan: { apk: path.join(dir, 'none.apk'), signed: true }, packageDir, gameName: 'Tri' }), /Gradle did not produce/);
});

test('build --target android --dry-run prints the Gradle command, and refuses --pack folder', async () => {
  const projectDir = makeProject({ name: 'Tri', build: { targets: { android: { appId: 'com.example.tri' } } } });
  const { reporter, lines } = capture();
  const code = await main(['build', '--target', 'android', '--project', projectDir, '--config', 'debug', '--dry-run', '--json'], { ctx: androidProbe(), reporter });
  assert.equal(code, 0, JSON.stringify(lines.at(-1)));
  const logs = lines.filter((l) => l.type === 'log').map((l) => l.text);
  assert.ok(logs.some((t) => t.startsWith('gradle -p ') && t.includes('assembleDebug') && t.includes('-Pn2.appId=com.example.tri')), logs.join('\n'));
  assert.ok(logs.some((t) => t.startsWith('game: zip pack ') && t.endsWith('game.n2pack')));
  assert.ok(!fs.existsSync(path.join(projectDir, '.n2')));

  const refused = capture();
  assert.equal(await main(['build', '--target', 'android', '--project', projectDir, '--pack', 'folder', '--dry-run', '--json'], { ctx: androidProbe(), reporter: refused.reporter }), 1);
  assert.match(refused.lines.at(-1).error, /one pack/);
});

// --- Android deploy (build targets stage 9) ------------------------------------------------------------------------

const KEYSTORE = path.resolve('/keys/game.jks');
const PASSWORD_FILE = path.resolve('/keys/key.pass');

function signingSettings(android) {
  return { signingProfiles: { release: { android: { keystore: KEYSTORE, keyAlias: 'upload', ...android } } } };
}

const signingIo = (env = {}, files = {}) => ({
  env,
  exists: (file) => file === KEYSTORE || Object.hasOwn(files, file),
  readFile: (file) => (Object.hasOwn(files, file) ? files[file] : null),
});

test('the signing profile name: --signing, else the project, else none; a bad name is refused', () => {
  assert.equal(signingProfileName({}, {}), null);
  assert.equal(signingProfileName({}, { signing: { profile: 'release' } }), 'release');
  assert.equal(signingProfileName({ signing: 'play' }, { signing: { profile: 'release' } }), 'play');
  assert.throws(() => signingProfileName({ signing: 'a b' }, {}), /profile name/);
});

test('a signing profile: passwords from text, the environment or a file; the key password defaults to the store one', () => {
  const fromEnv = resolveAndroidSigning('release', signingSettings({ storePassword: { env: 'STORE_PW' } }), signingIo({ STORE_PW: 's3cret' }));
  assert.deepEqual(fromEnv, { profile: 'release', storeFile: KEYSTORE, keyAlias: 'upload', storePassword: 's3cret', keyPassword: 's3cret' });
  const fromFile = resolveAndroidSigning('release', signingSettings({ storePassword: 'text', keyPassword: { file: PASSWORD_FILE } }),
    signingIo({}, { [PASSWORD_FILE]: 'k3y\nsecond line ignored\n' }));
  assert.equal(fromFile.keyPassword, 'k3y');
  assert.equal(fromFile.storePassword, 'text');
});

test('a signing profile that is missing or wrong is refused with the reason, never a password', () => {
  const io = signingIo({ STORE_PW: 'hunter2' });
  const fails = (settings, pattern, name = 'release', ioOverride = io) => {
    assert.throws(() => resolveAndroidSigning(name, settings, ioOverride), (error) => {
      assert.ok(error instanceof BuildError);
      assert.match(error.message, pattern);
      assert.ok(!error.message.includes('hunter2'), error.message);
      return true;
    });
  };
  fails({}, /"release", which is not in your settings/);
  fails(signingSettings({ storePassword: { env: 'STORE_PW' } }), /"play", which is not/, 'play');
  fails({ signingProfiles: { release: {} } }, /no "android" part/);
  fails(signingSettings({ storePassword: { env: 'NOT_SET' } }), /NOT_SET is not set/);
  fails(signingSettings({ storePassword: { env: 'STORE_PW' }, keystore: 'relative.jks' }), /absolute path/);
  fails(signingSettings({ storePassword: { env: 'STORE_PW' }, keystore: path.resolve('/keys/missing.jks') }), /does not exist/);
  fails(signingSettings({ storePassword: { env: 'STORE_PW' }, keyAlias: '' }), /keyAlias/);
  fails(signingSettings({ storePassword: { file: PASSWORD_FILE } }), /cannot be read/);
  fails(signingSettings({ storePassword: { file: 'relative.pass' } }), /absolute path/);
  fails(signingSettings({ storePassword: { env: 'STORE_PW', file: PASSWORD_FILE } }), /must be a string/);
  fails(signingSettings({ storePassword: '' }), /is empty/);
  fails(signingSettings({ storePassword: 'x', certificate: 'y' }), /unknown key "certificate"/);
});

test('signing reaches Gradle in its environment only, and nothing else gets it', () => {
  const signing = { storeFile: KEYSTORE, keyAlias: 'upload', storePassword: 'a', keyPassword: 'b' };
  const env = androidSigningEnv(signing);
  assert.deepEqual(env, { [ANDROID_SIGNING_ENV.storeFile]: KEYSTORE, [ANDROID_SIGNING_ENV.storePassword]: 'a',
    [ANDROID_SIGNING_ENV.keyAlias]: 'upload', [ANDROID_SIGNING_ENV.keyPassword]: 'b' });
  assert.deepEqual(withoutSigningEnv({ PATH: '/bin', ...env }), { PATH: '/bin' });
  const gradle = fs.readFileSync(path.join(ENGINE_ROOT, 'players', 'android', 'app', 'build.gradle'), 'utf8');
  for (const name of Object.values(ANDROID_SIGNING_ENV)) assert.ok(gradle.includes(`signingEnv('${name}')`), `build.gradle reads ${name}`);
  assert.ok(!/findProperty\(["']n2\.(store|key)/i.test(gradle), 'no signing value comes from a -P property');
});

test('the reporter hides a secret in every line, JSON-escaped spellings included', () => {
  const { reporter, lines } = capture();
  reporter.addSecret('pa"ss\\word');
  reporter.addSecret('pa');
  reporter.log('stdout', 'storePassword=pa"ss\\word and pa');
  reporter.warning('pa');
  reporter.result({ ok: false, error: 'bad pa"ss\\word' });
  const text = lines.map((l) => JSON.stringify(l)).join('\n');
  assert.ok(!text.includes('pa"ss') && !text.includes('ss\\\\word') && !/\bpa\b/.test(text), text);
  assert.equal(lines[0].text, 'storePassword=**** and ****');
  const human = [];
  const plain = new Reporter({ out: (l) => human.push(l), err: (l) => human.push(l) });
  plain.addSecret('topsecret');
  plain.log('stderr', 'echo topsecret');
  assert.deepEqual(human, ['echo ****']);
});

test('a release plan with a profile signs the APK and builds the AAB; without one it is unsigned and has no AAB', () => {
  const projectDir = makeProject({ name: 'Tri' });
  const base = { engineRoot: ENGINE_ROOT, projectDir, descriptor: androidDescriptor(), modules: undefined, buildRoot: tmp('n2build-android-'),
    gameName: 'Tri', settings: {}, info: { androidSdk: '/sdk', androidNdk: '27.0.12077973' }, cmakeDir: '/usr' };
  const signed = planAndroid({ ...base, config: 'release', signingProfile: 'release' });
  assert.deepEqual(signed.tasks, ['assembleRelease', 'bundleRelease']);
  assert.ok(signed.apk.endsWith(path.join('apk', 'release', 'app-release.apk')));
  assert.ok(signed.aab.endsWith(path.join('bundle', 'release', 'app-release.aab')));
  assert.equal(signed.signed, true);
  assert.equal(signed.signingProfile, 'release');
  assert.ok(!signed.gradleArgs.some((a) => /keystore|password|\.jks/i.test(a)), 'no signing detail on the command line');
  const unsigned = planAndroid({ ...base, config: 'release' });
  assert.deepEqual(unsigned.tasks, ['assembleRelease']);
  assert.equal(unsigned.aab, null);
  assert.equal(unsigned.signed, false);
  const debug = planAndroid({ ...base, config: 'debug', signingProfile: 'release' });
  assert.deepEqual(debug.tasks, ['assembleDebug']);
  assert.equal(debug.signingProfile, null, 'a debug build uses the debug key');
});

test('build --dry-run names the signing profile without reading it', async () => {
  const projectDir = makeProject({ name: 'Tri', build: { targets: { android: { signing: { profile: 'release' } } } } });
  const { reporter, lines } = capture();
  const code = await main(['build', '--target', 'android', '--project', projectDir, '--dry-run', '--json'], { ctx: androidProbe(), reporter });
  assert.equal(code, 0, JSON.stringify(lines.at(-1)));
  const logs = lines.filter((l) => l.type === 'log').map((l) => l.text);
  assert.ok(logs.some((t) => t.includes('assembleRelease') && t.includes('bundleRelease')), logs.join('\n'));
  assert.ok(logs.includes('signing: the profile "release" (read when the build runs)'));
  assert.ok(logs.some((t) => t.startsWith('aab: ')));
});

test('a build whose signing profile is missing fails before anything is built', async () => {
  const projectDir = makeProject({ name: 'Tri', build: { targets: { android: { signing: { profile: 'release' } } } } });
  const { reporter, lines } = capture();
  const home = tmp('n2build-home-');
  const code = await main(['build', '--target', 'android', '--project', projectDir, '--build-dir', tmp('n2build-tree-'), '--json'],
    { ctx: androidProbe({ env: { HOME: home, XDG_DATA_HOME: path.join(home, 'data') } }), reporter });
  assert.equal(code, 1);
  assert.match(lines.at(-1).error, /signing profile "release", which is not in your settings/);
});

test('adb: the SDK\'s adb, the device list, and which device a run uses', () => {
  assert.equal(findAdb('/sdk', { platform: 'linux', exists: (f) => f === '/sdk/platform-tools/adb' }), '/sdk/platform-tools/adb');
  assert.equal(findAdb('C:\\sdk', { platform: 'win32', exists: (f) => f === 'C:\\sdk\\platform-tools\\adb.exe' }), 'C:\\sdk\\platform-tools\\adb.exe');
  assert.equal(findAdb(null, { platform: 'linux', exists: () => true }), null);
  const devices = parseDevices([
    '* daemon not running; starting now at tcp:5037', 'List of devices attached',
    'R5CT1234\tdevice usb:1-1 product:a54 model:SM_A546B device:a54x transport_id:2',
    'emulator-5554   device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emu64x transport_id:1',
    'ZY22\tunauthorized usb:1-2 transport_id:3', '',
  ].join('\r\n'));
  assert.deepEqual(devices, [
    { serial: 'R5CT1234', state: 'device', model: 'SM_A546B' },
    { serial: 'emulator-5554', state: 'device', model: 'sdk_gphone64_x86_64' },
    { serial: 'ZY22', state: 'unauthorized', model: null },
  ]);
  assert.throws(() => pickDevice(devices), /Several devices are attached: choose one with --device \(R5CT1234 \(SM_A546B\), emulator-5554/);
  assert.equal(pickDevice(devices, 'emulator-5554').serial, 'emulator-5554');
  assert.throws(() => pickDevice(devices, 'ZY22'), /unauthorized: accept the USB debugging prompt/);
  assert.throws(() => pickDevice(devices, 'nope'), /No device nope is attached/);
  assert.equal(pickDevice(devices.slice(0, 1)).serial, 'R5CT1234');
  assert.throws(() => pickDevice([]), /No Android device is attached/);
  assert.throws(() => pickDevice(devices.slice(2)), /ZY22 is unauthorized/);
});

/** A fake adb: answers each command, records them; the game's process lives for `alive` pidof calls */
function fakeAdb({ alive = 3, install = { code: 0, stdout: 'Success', stderr: '' } } = {}) {
  const calls = [];
  let pidofCalls = 0;
  const exec = async (adb, args) => {
    calls.push(args.slice(2).join(' '));
    if (args[2] === 'install') return install;
    if (args.includes('pidof')) {
      pidofCalls += 1;
      return pidofCalls <= alive ? { code: 0, stdout: '4242\n', stderr: '' } : { code: 1, stdout: '', stderr: '' };
    }
    return { code: 0, stdout: '', stderr: '' };
  };
  let stopped = false;
  const stream = (adb, args, { onLine }) => {
    calls.push(args.slice(2).join(' '));
    onLine('I/N2Engine( 4242): N2Engine Android player: first frame');
    return { stop: () => { stopped = true; }, done: Promise.resolve() };
  };
  return { calls, exec, stream, wasStopped: () => stopped };
}

test('runOnDevice: install, start, the game\'s logcat as log events, until its process ends', async () => {
  const adb = fakeAdb({ alive: 3 });
  const { reporter, lines } = capture();
  const outcome = await runOnDevice({ adb: 'adb', apk: path.join('out', 'Tri.apk'), appId: 'com.example.tri', device: { serial: 'emulator-5554', state: 'device', model: null },
    env: {}, reporter, exec: adb.exec, stream: adb.stream, sleep: async () => {} });
  assert.deepEqual(outcome, { pid: '4242', reason: 'exited' });
  assert.deepEqual(adb.calls.slice(0, 4), [`install -r ${path.join('out', 'Tri.apk')}`, 'logcat -c', `shell am start -W -n com.example.tri/${ACTIVITY}`, 'shell pidof com.example.tri']);
  assert.ok(adb.calls.includes('logcat -v brief --pid=4242'));
  assert.ok(adb.wasStopped());
  assert.deepEqual(lines.filter((l) => l.type === 'log').map((l) => [l.stream, l.text]), [['logcat', 'I/N2Engine( 4242): N2Engine Android player: first frame']]);
  assert.deepEqual(lines.filter((l) => l.type === 'step').map((l) => l.id), ['install', 'run', 'logcat']);
});

test('runOnDevice: --seconds stops the game; a failed install is an error with adb\'s reason', async () => {
  const adb = fakeAdb({ alive: 1000 });
  let clock = 0;
  const outcome = await runOnDevice({ adb: 'adb', apk: 'Tri.apk', appId: 'com.example.tri', device: { serial: 's', state: 'device' }, env: {},
    reporter: capture().reporter, seconds: 5, exec: adb.exec, stream: adb.stream, sleep: async (ms) => { clock += ms; }, now: () => clock });
  assert.equal(outcome.reason, 'time');
  assert.ok(adb.calls.includes('shell am force-stop com.example.tri'));

  const refused = fakeAdb({ install: { code: 1, stdout: 'Performing Streamed Install\nadb: failed to install Tri.apk: Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES]', stderr: '' } });
  await assert.rejects(runOnDevice({ adb: 'adb', apk: 'Tri.apk', appId: 'com.example.tri', device: { serial: 's', state: 'device' }, env: {},
    reporter: capture().reporter, exec: refused.exec, stream: refused.stream, sleep: async () => {} }), /adb install failed: .*INSTALL_PARSE_FAILED_NO_CERTIFICATES/);
});

test('run --target android refuses an unsigned package, and picks the device through adb', async () => {
  const packageDir = tmp('n2build-apk-run-');
  write(path.join(packageDir, '.n2build-package.json'), { target: 'android', config: 'release', entry: 'Tri-unsigned.apk', appId: 'com.example.tri', signed: false });
  const projectDir = makeProject({ name: 'Tri' });
  const unsigned = capture();
  assert.equal(await main(['run', '--target', 'android', '--project', projectDir, '--out', packageDir, '--no-build', '--json'], { ctx: androidProbe(), reporter: unsigned.reporter }), 1);
  assert.match(unsigned.lines.at(-1).error, /unsigned, and Android installs only signed APKs/);

  write(path.join(packageDir, '.n2build-package.json'), { target: 'android', config: 'debug', entry: 'Tri.apk', appId: 'com.example.tri', signed: true });
  const adb = fakeAdb({ alive: 2 });
  const ctx = androidProbe({ files: ['/sdk/platform-tools/adb'], commands: { adb: 'List of devices attached\nemulator-5554\tdevice model:emu\n' } });
  ctx.adb = { exec: adb.exec, stream: adb.stream, sleep: async () => {} };
  const ran = capture();
  assert.equal(await main(['run', '--target', 'android', '--project', projectDir, '--out', packageDir, '--no-build', '--json'], { ctx, reporter: ran.reporter }), 0, JSON.stringify(ran.lines.at(-1)));
  assert.deepEqual(ran.lines.at(-1), { type: 'result', ok: true, output: packageDir, device: 'emulator-5554', pid: '4242', ended: 'exited' });
  assert.ok(adb.calls[0].startsWith('install -r ') && adb.calls[0].endsWith('Tri.apk'));
});

// --- Apple targets (build targets stage 10) ------------------------------------------------------------------------

test('the apple descriptors: ios is an app built with Xcode, macos a desktop player; both build in GitHub Actions off a Mac', () => {
  const descriptors = loadDescriptors(ENGINE_ROOT);
  const ios = descriptors.get('ios');
  assert.equal(ios.package.kind, 'ios');
  assert.equal(ios.cmake.generator, 'Xcode');
  assert.equal(ios.cmake.options.CMAKE_SYSTEM_NAME, 'iOS');
  assert.deepEqual(ios.physics3d, ['none', 'jolt']);
  assert.equal(ios.remote.workflow, 'n2build-apple.yml');
  assert.equal(descriptors.get('macos').package.kind, 'desktop');
  assert.ok(buildsRemotely(ios, 'win32') && buildsRemotely(ios, 'linux') && !buildsRemotely(ios, 'darwin'));
  assert.ok(!buildsRemotely(descriptors.get('linux'), 'win32'), 'a target without remote never builds remotely');
  const bad = JSON.parse(fs.readFileSync(path.join(ENGINE_ROOT, 'targets', 'ios', 'target.json'), 'utf8'));
  assert.throws(() => validateDescriptor({ ...bad, remote: { ...bad.remote, kind: 'ssh' } }, 'x'), /remote\.kind/);
  assert.throws(() => validateDescriptor({ ...bad, remote: { ...bad.remote, workflow: '../evil.yml' } }, 'x'), /remote\.workflow/);
});

test('doctor off a Mac: ios checks only gh and its login, and says the build runs in GitHub Actions', () => {
  const descriptor = loadDescriptors(ENGINE_ROOT).get('ios');
  const loggedIn = fakeProbe({ platform: 'win32', env: { PATH: 'C:\\gh', PATHEXT: '.EXE' }, files: ['C:\\gh\\gh.exe'],
    commands: { gh: (args) => (args[0] === '--version' ? { status: 0, stdout: 'gh version 2.63.0 (2026-10-01)', stderr: '' } : { status: 0, stdout: '', stderr: 'Logged in to github.com account someone' }) } });
  const ok = checkedToolchains(descriptor, loggedIn);
  assert.deepEqual(ok.checks.map((c) => [c.id, c.ok]), [['gh', true]]);
  assert.equal(ok.checks[0].version, '2.63.0');
  const loggedOut = fakeProbe({ platform: 'linux', files: ['/usr/bin/gh'], commands: { gh: (args) => (args[0] === '--version' ? 'gh version 2.63.0' : { status: 1, stdout: '', stderr: 'You are not logged into any GitHub hosts' }) } });
  const notOk = checkedToolchains(descriptor, loggedOut).checks[0];
  assert.equal(notOk.ok, false);
  assert.match(notOk.fix, /gh auth login/);
  const onMac = checkedToolchains(descriptor, fakeProbe({ platform: 'darwin', files: ['/usr/bin/cmake'], commands: { xcodebuild: 'Xcode 16.2\nBuild version 16C5032a', cmake: 'cmake version 3.31.0' } }));
  assert.deepEqual(onMac.checks.map((c) => [c.id, c.ok, c.version]), [['cmake', true, '3.31.0'], ['xcode', true, '16.2']]);
});

test('ios settings and configure options; the Xcode build names its configuration', () => {
  assert.deepEqual(iosSettings({}, 'Tri'), { appId: 'com.n2engine.tri', orientation: 'any' });
  assert.throws(() => iosSettings({ appId: 'tri' }, 'Tri'), /appId/);
  assert.throws(() => iosSettings({ orientation: 'up' }, 'Tri'), /orientation/);
  const args = iosConfigureArgs({ settings: { appId: 'com.example.tri', version: '1.2.3', buildNumber: 9, orientation: 'landscape', displayName: 'Tri Like' }, gameName: 'Tri', packFile: '/tree/game.n2pack' });
  assert.deepEqual(args, ['-DN2ENGINE_IOS_PACK=/tree/game.n2pack', '-DN2ENGINE_IOS_BUNDLE_ID=com.example.tri', '-DN2ENGINE_IOS_NAME=Tri Like',
    '-DN2ENGINE_IOS_VERSION=1.2.3', '-DN2ENGINE_IOS_BUILD=9', '-DN2ENGINE_IOS_ORIENTATION=landscape']);
  const descriptor = loadDescriptors(ENGINE_ROOT).get('ios');
  const plan = planBuild({ engineRoot: ENGINE_ROOT, projectDir: makeProject({ name: 'Tri' }), descriptor, config: 'release', modules: undefined, present: new Set(),
    buildRoot: tmp('n2build-ios-'), gameName: 'Tri', extraConfigureArgs: args });
  assert.ok(plan.configureArgs.includes('Xcode'));
  assert.ok(plan.configureArgs.includes('-DCMAKE_SYSTEM_NAME=iOS'));
  assert.ok(plan.configureArgs.includes('-DN2ENGINE_IOS_BUNDLE_ID=com.example.tri'));
  assert.deepEqual(plan.buildArgs.slice(2, 6), ['--target', 'n2ios', '--config', 'Release']);
  assert.equal(packPath({ descriptor, packageDir: '/out', treeDir: plan.treeDir }), path.join(plan.treeDir, 'game.n2pack'));
});

test('the iOS package: the app Xcode built, found in its configuration\'s folder, named after the game', () => {
  const tree = tmp('n2build-ios-tree-');
  write(path.join(tree, 'players', 'ios', 'Debug-iphonesimulator', 'N2Game.app', 'N2Game'), 'debug');
  write(path.join(tree, 'players', 'ios', 'Release-iphonesimulator', 'N2Game.app', 'N2Game'), 'release');
  write(path.join(tree, 'players', 'ios', 'Release-iphonesimulator', 'N2Game.app', 'game.n2pack'), 'PK');
  assert.ok(findAppBundle(tree, 'release').includes('Release-iphonesimulator'));
  assert.ok(findAppBundle(tree, 'debug').includes('Debug-iphonesimulator'));
  const packageDir = tmp('n2build-ios-out-');
  assert.equal(packageIos({ treeDir: tree, config: 'release', packageDir, gameName: 'Tri Like!' }), 'Tri Like_.app');
  assert.equal(fs.readFileSync(path.join(packageDir, 'Tri Like_.app', 'N2Game'), 'utf8'), 'release');
  assert.ok(fs.existsSync(path.join(packageDir, 'Tri Like_.app', 'game.n2pack')));
  assert.throws(() => packageIos({ treeDir: tmp('n2build-empty-'), config: 'release', packageDir, gameName: 'Tri' }), /did not produce the app/);
});

test('the simulator: the booted one, else the newest iPhone; install, launch, the console as log events', async () => {
  const list = (booted) => JSON.stringify({ devices: {
    'com.apple.CoreSimulator.SimRuntime.iOS-17-5': [{ udid: 'OLD', name: 'iPhone 15', state: 'Shutdown', isAvailable: true }],
    'com.apple.CoreSimulator.SimRuntime.iOS-18-2': [{ udid: 'IPAD', name: 'iPad Air', state: 'Shutdown', isAvailable: true },
      { udid: 'NEW', name: 'iPhone 16', state: booted ? 'Booted' : 'Shutdown', isAvailable: true }],
    'com.apple.CoreSimulator.SimRuntime.watchOS-11-2': [{ udid: 'W', name: 'Apple Watch', state: 'Booted' }],
  } });
  assert.deepEqual(pickSimulator(list(false)), { udid: 'NEW', name: 'iPhone 16', booted: false });
  assert.deepEqual(pickSimulator(list(true)), { udid: 'NEW', name: 'iPhone 16', booted: true });
  assert.equal(pickSimulator('{"devices": {}}'), null);
  assert.equal(pickSimulator('not json'), null);

  // The game's log comes through the simulator's log stream (the app's process, its N2Engine lines); the app is a process
  // of this Mac for two checks, then is gone
  const calls = [];
  let checks = 0;
  let stopped = false;
  const exec = async (command, args) => {
    calls.push(args.join(' '));
    if (args[1] === 'list') return { code: 0, stdout: list(false), stderr: '' };
    if (args[1] === 'launch') return { code: 0, stdout: 'com.example.tri: 4242\n', stderr: '' };
    return { code: 0, stdout: '', stderr: '' };
  };
  const stream = (command, args, { onLine }) => {
    calls.push(args.join(' '));
    onLine('Timestamp               Ty Process[PID:TID]');
    onLine('2026-11-12 13:06:58.898 Df N2Game[4242:181b4] N2Engine info: N2Engine iOS player: first frame');
    onLine('2026-11-12 13:07:05.125 Df N2Game[4242:181b4] N2Engine info: N2Engine iOS player: 15 frames, 3.0 fps');
    return { stop: () => { stopped = true; }, done: Promise.resolve() };
  };
  const isAlive = (pid) => {
    assert.equal(pid, 4242, 'the pid simctl launch printed');
    checks += 1;
    return checks <= 2;
  };
  const { reporter, lines } = capture();
  const outcome = await runInSimulator({ app: '/out/Tri.app', bundleId: 'com.example.tri', env: {}, reporter, exec, stream, isAlive, sleep: async () => {} });
  assert.deepEqual(outcome, { device: 'NEW', pid: '4242', reason: 'exited' });
  assert.deepEqual(calls.slice(0, 6), ['simctl list devices available --json', 'simctl boot NEW', 'simctl bootstatus NEW -b',
    'simctl install NEW /out/Tri.app',
    'simctl spawn NEW log stream --style compact --level info --predicate process == "N2Game" AND eventMessage BEGINSWITH "N2Engine"',
    'simctl launch --terminate-running-process NEW com.example.tri']);
  assert.equal(checks, 3, 'the process is checked until it is gone');
  assert.ok(stopped, 'the log stream is stopped at the end');
  assert.ok(!calls.some((c) => c.includes('terminate NEW')), 'an app that exited is not terminated');
  assert.deepEqual(lines.filter((l) => l.type === 'log').map((l) => [l.stream, l.text]), [
    ['console', 'N2Engine info: N2Engine iOS player: first frame'], ['console', 'N2Engine info: N2Engine iOS player: 15 frames, 3.0 fps']]);

  // --seconds: the app is stopped when the time is up
  const timed = [];
  const keepRunning = async (command, args) => {
    timed.push(args.join(' '));
    if (args[1] === 'list') return { code: 0, stdout: list(true), stderr: '' };
    return { code: 0, stdout: 'com.example.tri: 7\n', stderr: '' };
  };
  const quiet = () => ({ stop: () => {}, done: Promise.resolve() });
  const timedOut = await runInSimulator({ app: '/out/Tri.app', bundleId: 'com.example.tri', env: {}, reporter: capture().reporter, exec: keepRunning,
    stream: quiet, isAlive: () => true, seconds: 3, sleep: async () => {} });
  assert.equal(timedOut.reason, 'time');
  assert.ok(timed.includes('simctl terminate NEW com.example.tri'));
  assert.ok(!timed.includes('simctl boot NEW'), 'a booted simulator is not booted again');
  assert.ok(timed.includes('simctl bootstatus NEW -b'), 'but it is waited for until it is ready');
});

/** A fake git for the game's repository */
function fakeGit({ dirty = '', pushed = 'abc123', head = 'abc123', branch = 'main', remote = 'git@github.com:me/game.git', root = '/repo' } = {}) {
  return (args) => {
    const command = args.slice(2).join(' ');
    if (command === 'rev-parse --show-toplevel') return { status: 0, stdout: `${root}\n` };
    if (command === 'rev-parse --abbrev-ref HEAD') return { status: 0, stdout: `${branch}\n` };
    if (command === 'rev-parse HEAD') return { status: 0, stdout: `${head}\n` };
    if (command.startsWith('rev-parse --verify --quiet origin/')) return pushed ? { status: 0, stdout: `${pushed}\n` } : { status: 1, stdout: '' };
    if (command.startsWith('status --porcelain')) return { status: 0, stdout: dirty };
    if (command === 'remote get-url origin') return remote ? { status: 0, stdout: `${remote}\n` } : { status: 2, stdout: '' };
    return null;
  };
}

test('a remote build needs the game committed and pushed to GitHub', () => {
  const projectDir = path.resolve('/repo/games/tri');
  const state = gitState(projectDir, fakeGit({ root: path.resolve('/repo') }));
  assert.equal(state.projectPath, 'games/tri');
  assert.equal(state.branch, 'main');
  assert.doesNotThrow(() => checkPushed(state));
  assert.throws(() => checkPushed(gitState(projectDir, fakeGit({ root: path.resolve('/repo'), dirty: ' M assets/scene.lua\n' }))), /not committed/);
  assert.throws(() => checkPushed(gitState(projectDir, fakeGit({ root: path.resolve('/repo'), pushed: null }))), /not on GitHub yet: push it \(git push -u origin main\)/);
  assert.throws(() => checkPushed(gitState(projectDir, fakeGit({ root: path.resolve('/repo'), head: 'def456' }))), /not pushed/);
  assert.throws(() => checkPushed(gitState(projectDir, fakeGit({ root: path.resolve('/repo'), remote: 'https://gitlab.com/me/game.git' }))), /no GitHub remote/);
  assert.throws(() => gitState(projectDir, () => ({ status: 128, stdout: '' })), /not in a git repository/);
  assert.throws(() => gitState(projectDir, fakeGit({ branch: 'HEAD' })), /detached HEAD/);
});

test('the workflow run: dispatched with plain inputs, found, followed, its artifact downloaded', async () => {
  const descriptor = loadDescriptors(ENGINE_ROOT).get('ios');
  assert.deepEqual(workflowInputs({ target: 'ios', config: 'release', projectPath: 'games/tri', engineRef: 'e1' }),
    ['-f', 'target=ios', '-f', 'config=release', '-f', 'project=games/tri', '-f', 'engine_ref=e1']);
  const t0 = Date.parse('2026-11-11T10:00:00Z');
  const runs = JSON.stringify([{ databaseId: 1, createdAt: '2026-11-11T09:00:00Z', url: 'u1' }, { databaseId: 2, createdAt: '2026-11-11T10:00:03Z', url: 'u2' }]);
  assert.equal(pickRun(runs, t0).databaseId, 2);
  assert.equal(pickRun('[]', t0), null);

  const calls = [];
  const exec = async (args) => {
    calls.push(args.join(' '));
    return args[0] === 'run' && args[1] === 'list' ? { code: 0, stdout: runs, stderr: '' } : { code: 0, stdout: '', stderr: '' };
  };
  const stream = async (args, onLine) => { calls.push(args.join(' ')); onLine('build: ✓'); return 0; };
  const { reporter, lines } = capture();
  const outcome = await dispatchAndFollow({ descriptor, state: { branch: 'main', projectPath: 'games/tri' }, config: 'release', engineRef: 'e1',
    packageDir: '/out', reporter, exec, stream, sleep: async () => {}, now: () => t0 });
  assert.deepEqual(outcome, { run: 2, url: 'u2' });
  assert.equal(calls[0], 'workflow run n2build-apple.yml --ref main -f target=ios -f config=release -f project=games/tri -f engine_ref=e1');
  assert.ok(calls.includes('run watch 2 --exit-status --interval 10'));
  assert.equal(calls.at(-1), 'run download 2 --name n2build-ios --dir /out');
  assert.deepEqual(lines.filter((l) => l.type === 'step').map((l) => l.id), ['dispatch', 'remote', 'download']);

  const failing = async (args) => { calls.push(args.join(' ')); return 1; };
  await assert.rejects(dispatchAndFollow({ descriptor, state: { branch: 'main', projectPath: '.' }, config: 'debug', packageDir: '/out',
    reporter: capture().reporter, exec, stream: failing, sleep: async () => {}, now: () => t0 }), /GitHub Actions build failed: u2/);
  const missing = async () => ({ code: 1, stdout: '', stderr: 'HTTP 404: workflow n2build-apple.yml not found' });
  await assert.rejects(dispatchAndFollow({ descriptor, state: { branch: 'main', projectPath: '.' }, config: 'debug', packageDir: '/out',
    reporter: capture().reporter, exec: missing, stream, sleep: async () => {}, now: () => t0 }), /n2build apple-workflow writes it/);
});

test('the workflow template: dispatch inputs only, a pinned macOS runner, n2build build and the simulator run, the artifact', () => {
  const text = workflowTemplate();
  assert.ok(text.startsWith('# Written by n2build apple-workflow'));
  for (const piece of ['workflow_dispatch:', 'runs-on: macos-15', 'n2build.mjs build --target', 'n2build.mjs run --target ios',
    "name: 'n2build-${{ inputs.target }}'", 'repository: EpicNicks/N2Engine', 'include-hidden-files: true']) {
    assert.ok(text.includes(piece), piece);
  }
  assert.ok(!/\$\{\{\s*secrets\./.test(text), 'no secret is used by the simulator workflow');
});

test('build --target ios --dry-run off a Mac prints the workflow it would start', async () => {
  const projectDir = makeProject({ name: 'Tri' });
  const ctx = fakeProbe({ platform: 'linux', files: ['/usr/bin/gh'], commands: { gh: (args) => (args[0] === '--version' ? 'gh version 2.63.0' : { status: 0, stdout: '', stderr: '' }) } });
  const git = fakeGit({ root: path.dirname(projectDir) });
  const baseRun = ctx.run;
  ctx.run = (command, args, options) => (command === 'git' ? git(args) : baseRun(command, args, options));
  const { reporter, lines } = capture();
  const code = await main(['build', '--target', 'ios', '--project', projectDir, '--dry-run', '--json'], { ctx, reporter });
  assert.equal(code, 0, JSON.stringify(lines.at(-1)));
  const logs = lines.filter((l) => l.type === 'log').map((l) => l.text);
  assert.ok(logs.some((t) => t.startsWith('gh workflow run n2build-apple.yml --ref main -f target=ios -f config=release -f project=')), logs.join('\n'));
  assert.equal(lines.at(-1).remote, true);
});

// --- private targets (build targets stage 11) ----------------------------------------------------------------------

const SAMPLE_TARGETS = path.join(ENGINE_ROOT, 'samples', 'private-targets');

test('a target folder outside targets/ adds a target with its own code: the sample console', () => {
  const descriptors = loadDescriptors(ENGINE_ROOT, [SAMPLE_TARGETS]);
  const sample = descriptors.get('sample-console');
  assert.ok(sample, 'the sample loads from its folder');
  assert.equal(sample.source, SAMPLE_TARGETS);
  assert.equal(sample.cmake.sourceDir, 'player');
  assert.ok(!loadDescriptors(ENGINE_ROOT).has('sample-console'), 'not one of the repository\'s targets');
  const plan = planBuild({ engineRoot: ENGINE_ROOT, projectDir: makeProject({ name: 'Tri' }), descriptor: sample, config: 'release', modules: undefined,
    present: new Set(), buildRoot: tmp('n2build-private-'), gameName: 'Tri' });
  assert.ok(plan.configureArgs.includes(`-DN2ENGINE_EXTRA_TARGET_DIR=${path.join(SAMPLE_TARGETS, 'sample-console', 'player')}`), plan.configureArgs.join(' '));
  assert.ok(plan.buildArgs.includes('sample_console_player'));
  assert.ok(fs.existsSync(path.join(SAMPLE_TARGETS, 'sample-console', 'player', 'CMakeLists.txt')));
});

test('a private target\'s cmake files stay inside its folder; a toolchain file is passed to CMake', () => {
  const base = JSON.parse(fs.readFileSync(path.join(SAMPLE_TARGETS, 'sample-console', 'target.json'), 'utf8'));
  for (const bad of ['../engine', '/abs/player', 'player/../../x', 'a\\..\\b', '']) {
    assert.throws(() => validateDescriptor({ ...base, cmake: { ...base.cmake, sourceDir: bad } }, 'x'), /cmake\.sourceDir must be a path inside/, bad);
  }
  assert.throws(() => validateDescriptor({ ...base, cmake: { ...base.cmake, toolchainFile: '../t.cmake' } }, 'x'), /cmake\.toolchainFile/);
  assert.throws(() => validateDescriptor({ ...base, cmake: { ...base.cmake, toolchain: 'emsdk', toolchainFile: 't.cmake' } }, 'x'), /cannot both/);
  const folder = tmp('n2build-private-folder-');
  write(path.join(folder, 'console-x', 'target.json'), { ...base, name: 'console-x', cmake: { ...base.cmake, toolchainFile: 'sdk/toolchain.cmake' } });
  const descriptor = loadDescriptors(ENGINE_ROOT, [folder]).get('console-x');
  const plan = planBuild({ engineRoot: ENGINE_ROOT, projectDir: makeProject({ name: 'Tri' }), descriptor, config: 'debug', modules: undefined,
    present: new Set(), buildRoot: tmp('n2build-private-'), gameName: 'Tri' });
  assert.ok(plan.configureArgs.includes(`-DCMAKE_TOOLCHAIN_FILE=${path.join(folder, 'console-x', 'sdk', 'toolchain.cmake')}`));
  assert.ok(plan.configureArgs.includes(`-DN2ENGINE_EXTRA_TARGET_DIR=${path.join(folder, 'console-x', 'player')}`));
});

test('no engine source names a private target: the sample is added from outside, without editing the engine', () => {
  const names = /sample[-_]console|SampleConsole/;
  const offenders = [];
  const scan = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) scan(full);
      else if (/\.(c|cc|cpp|h|hpp|mm|cmake|txt|json|mjs|gradle)$/.test(entry.name) && names.test(fs.readFileSync(full, 'utf8'))) offenders.push(path.relative(ENGINE_ROOT, full));
    }
  };
  for (const dir of ['engine', 'renderer', 'math', 'text', 'players', 'cmake', 'targets', 'web', path.join('tools', 'n2build', 'lib')]) scan(path.join(ENGINE_ROOT, dir));
  if (names.test(fs.readFileSync(path.join(ENGINE_ROOT, 'CMakeLists.txt'), 'utf8'))) offenders.push('CMakeLists.txt');
  assert.deepEqual(offenders, []);
});

test('the Vulkan renderer on Android (stage 12): a Gradle property and its own build tree', () => {
  const projectDir = makeProject({ name: 'Tri' });
  const base = { engineRoot: ENGINE_ROOT, projectDir, descriptor: androidDescriptor(), modules: undefined, buildRoot: tmp('n2build-android-'),
    gameName: 'Tri', config: 'debug', info: { androidSdk: '/sdk', androidNdk: '27.0.12077973' }, cmakeDir: '/usr' };
  const software = planAndroid({ ...base, settings: {} });
  const vulkan = planAndroid({ ...base, settings: { renderer: 'vulkan' } });
  assert.ok(software.gradleArgs.includes('-Pn2.vulkan=OFF'));
  assert.ok(vulkan.gradleArgs.includes('-Pn2.vulkan=ON'));
  assert.notEqual(software.treeDir, vulkan.treeDir, 'switching the renderer never rebuilds the other tree');
  const gradle = fs.readFileSync(path.join(ENGINE_ROOT, 'players', 'android', 'app', 'build.gradle'), 'utf8');
  assert.ok(gradle.includes("-DN2ENGINE_ANDROID_VULKAN=${n2('vulkan', 'OFF')}"));
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
