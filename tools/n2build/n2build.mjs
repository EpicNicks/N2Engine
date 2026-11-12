#!/usr/bin/env node
// n2build: builds an N2Engine game for a target (build targets stage 1; docs/build-targets.html).
//
//   node tools/n2build/n2build.mjs list
//   node tools/n2build/n2build.mjs doctor --target <name> [--project DIR]
//   node tools/n2build/n2build.mjs build  --target <name> --project DIR [--config debug|release] [--out DIR]
//                                         [--build-dir DIR] [--archive zip|7z] [--jobs N] [--dry-run]
//   node tools/n2build/n2build.mjs run    --target <name> --project DIR [--config ...] [--no-build] [--frames N] [--port N]
//                                         [--device SERIAL] [--seconds N]
//   node tools/n2build/n2build.mjs cook   --target <name> --project DIR [--out FILE] [--pack zip|stored|7z]
//
// --pack (build, cook): how the game's files are packaged (docs/packs.html): zip, stored (an uncompressed zip), 7z
// (non-solid, through 7-Zip) or folder (the project folder as it is). Default: the target's build.targets pack, else the
// descriptor's package.pack, else zip for desktop and stored for the web.
//
// Common: --json (one JSON event per line on stdout, for the editor), --targets-dir DIR (more target descriptors; also
// N2BUILD_TARGET_DIRS and "targetFolders" in the user's n2build.json).
//
// The final game is always a full build from source (decision D3): the target's player is compiled with the project's
// module list in a build tree kept per target, configuration and list, then packaged with the game's data.
//
// Exit codes: 0 done, 1 the build or a check failed, 2 wrong usage.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import {
  archivePackage, checkPhysics, engineRevision, needsConfigure, packageDesktop, packageWeb, packBeforeConfigure, packPath, planBuild, safeName,
  preparePackageDir, readMarker, readProject, recordConfigure, resolvePackFormat, runProcess, writeMarker,
} from './lib/build.mjs';
import { findAdb, parseDevices, pickDevice, runOnDevice } from './lib/adb.mjs';
import { captureProcess, iosConfigureArgs, iosSettings, packageIos, runInSimulator } from './lib/apple.mjs';
import { checkPushed, dispatchAndFollow, gitState, workflowInputs, workflowTemplate } from './lib/remote.mjs';
import { cmakeInstallDir, gradleInvocation, packageAndroid, planAndroid, prepareGradleTree } from './lib/android.mjs';
import { PACK_EXTENSION, cookPack, findSevenZip } from './lib/cook.mjs';
import { PACK_KINDS } from './lib/descriptors.mjs';
import { findTarget, loadDescriptors, readUserSettings, userSettingsFile } from './lib/descriptors.mjs';
import { BuildError, Reporter } from './lib/report.mjs';
import { serveFolder } from './lib/serve.mjs';
import { androidSigningEnv, resolveAndroidSigning, signingProfileName, withoutSigningEnv } from './lib/signing.mjs';
import { checkToolchains, findOnPath, systemProbe } from './lib/toolchains.mjs';

export const ENGINE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const COMMANDS = ['list', 'doctor', 'build', 'run', 'cook', 'apple-workflow'];
const VALUE_OPTIONS = new Set(['--target', '--project', '--config', '--out', '--build-dir', '--archive', '--jobs', '--frames', '--port', '--targets-dir', '--pack',
  '--signing', '--device', '--seconds']);
const FLAG_OPTIONS = new Set(['--json', '--dry-run', '--no-build', '--help']);

export const USAGE = `Usage: n2build <command> [options]
  list                         the targets, and which build on this machine
  doctor --target T            what target T needs and whether this machine has it
  build  --target T --project DIR [--config debug|release] [--out DIR] [--build-dir DIR]
         [--archive zip|7z] [--jobs N] [--dry-run]
  run    --target T --project DIR [--config debug|release] [--no-build] [--frames N] [--port N]
         [--device SERIAL] [--seconds N]   (android: the device, and how long to follow the game's log)
  cook   --target T --project DIR [--out FILE] [--pack zip|stored|7z]
  apple-workflow --project DIR   writes .github/workflows/n2build-apple.yml into the game's repository (iOS and
         macOS build there when this machine is not a Mac; docs/apple.html#remote)
         build also takes --pack zip|stored|7z|folder (how the game's files are packaged)
         and --signing PROFILE (a signing profile in your n2build.json; default: the project's)
Common: --json (JSON lines for the editor), --targets-dir DIR (more targets)`;

export class UsageError extends Error {}

/** argv -> {command, options}; repeated --targets-dir accumulate */
export function parseArguments(argv) {
  const [command, ...rest] = argv;
  if (command === undefined || command === '--help' || command === '-h') return { command: 'help', options: {} };
  if (!COMMANDS.includes(command)) throw new UsageError(`Unknown command '${command}'`);
  const options = { targetsDir: [] };
  for (let i = 0; i < rest.length; ++i) {
    let arg = rest[i];
    let value;
    const eq = arg.indexOf('=');
    if (arg.startsWith('--') && eq > 0) {
      value = arg.slice(eq + 1);
      arg = arg.slice(0, eq);
    }
    if (FLAG_OPTIONS.has(arg)) {
      if (value !== undefined) throw new UsageError(`${arg} takes no value`);
      options[camel(arg)] = true;
    } else if (VALUE_OPTIONS.has(arg)) {
      if (value === undefined) {
        if (i + 1 >= rest.length) throw new UsageError(`${arg} needs a value`);
        value = rest[++i];
      }
      if (arg === '--targets-dir') options.targetsDir.push(value);
      else options[camel(arg)] = value;
    } else {
      throw new UsageError(`Unknown option '${arg}'`);
    }
  }
  // --config left out: the project's build.targets.<target>.config, else release (resolveConfig). A private target may
  // name other configurations; its descriptor decides which exist (resolveOptions)
  if (options.config !== undefined && !/^[a-z][a-z0-9-]{0,31}$/.test(options.config)) {
    throw new UsageError('--config must be a configuration name such as debug or release');
  }
  for (const [name, key] of [['--jobs', 'jobs'], ['--frames', 'frames'], ['--port', 'port'], ['--seconds', 'seconds']]) {
    if (options[key] !== undefined) {
      const number = Number(options[key]);
      const min = key === 'port' ? 0 : 1;
      if (!Number.isInteger(number) || number < min || number > 65535 * (key === 'port' ? 1 : 1000)) {
        throw new UsageError(`${name} needs a whole number${key === 'port' ? ' from 0 to 65535' : ', 1 or more'}`);
      }
      options[key] = number;
    }
  }
  if (options.archive !== undefined && !['zip', '7z'].includes(options.archive)) throw new UsageError('--archive must be zip or 7z');
  if (options.pack !== undefined && !PACK_KINDS.includes(options.pack)) throw new UsageError(`--pack must be one of ${PACK_KINDS.join(', ')}`);
  if (command === 'cook' && options.pack === 'folder') throw new UsageError('cook makes a pack: --pack zip, stored or 7z');
  if (command !== 'list' && command !== 'apple-workflow' && !options.target) throw new UsageError(`${command} needs --target`);
  if ((command === 'build' || command === 'run' || command === 'cook' || command === 'apple-workflow') && !options.project) throw new UsageError(`${command} needs --project`);
  return { command, options };
}

function camel(flag) {
  return flag.replace(/^--/, '').replace(/-([a-z])/g, (_, c) => c.toUpperCase());
}

/** The configuration to build: --config, else the target's settings in the project, else release */
export function resolveConfig(options, settings) {
  return options.config ?? settings?.config ?? 'release';
}

/** The package marker: what was built, for whom, and from which engine. No local paths: a web package is uploaded as it is */
export function markerInfo({ descriptor, config, entry, project, engine, builtAt }) {
  const info = { target: descriptor.name, config, entry, game: project.name };
  if (project.settings?.version !== undefined) info.version = project.settings.version;
  if (project.settings?.buildNumber !== undefined) info.buildNumber = project.settings.buildNumber;
  return { ...info, engine, builtAt };
}

/** The extra target folders: user settings, then N2BUILD_TARGET_DIRS, then --targets-dir */
export function extraTargetFolders(options, env, platform) {
  const settings = readUserSettings(userSettingsFile(env, platform));
  const fromEnv = (env.N2BUILD_TARGET_DIRS ?? '').split(path.delimiter).filter(Boolean);
  return [...(settings.targetFolders ?? []), ...fromEnv, ...options.targetsDir].map((f) => path.resolve(f));
}

/** Whether this machine builds the target in GitHub Actions: it cannot build it here, and the target says where else (D7) */
export function buildsRemotely(descriptor, platform) {
  return !descriptor.hostSystems.includes(platform) && descriptor.remote !== undefined;
}

/** The toolchains to check here: a remote build needs only its own (gh), the rest are the workflow runner's */
export function checkedToolchains(descriptor, ctx) {
  if (!buildsRemotely(descriptor, ctx.platform)) return checkToolchains(descriptor, ctx);
  return checkToolchains({ ...descriptor, toolchains: { required: descriptor.remote.toolchains ?? [], optional: [] } }, ctx);
}

function doctorReport(descriptor, toolchains, platform, physicsProblem) {
  const remote = buildsRemotely(descriptor, platform);
  const hostOk = descriptor.hostSystems.includes(platform) || remote;
  const checks = [
    { id: 'host', title: `Host system (${platform})`, required: true, ok: hostOk, version: null,
      detail: remote ? `builds in GitHub Actions (${descriptor.remote.workflow}; docs/apple.html#remote)` : hostOk ? '' : `${descriptor.name} builds on ${descriptor.hostSystems.join(', ')} only`,
      fix: hostOk ? null : `Build on ${descriptor.hostSystems.join(' or ')}` },
    ...toolchains.checks,
  ];
  if (physicsProblem !== undefined) {
    checks.push({ id: 'physics', title: 'Project physics modules', required: true, ok: physicsProblem === null, version: null, detail: physicsProblem ?? '', fix: physicsProblem ? 'Edit build.modules in project.n2proj' : null });
  }
  return { target: descriptor.name, displayName: descriptor.displayName, ok: checks.every((c) => c.ok || !c.required), checks };
}

async function build(descriptor, options, ctx, reporter) {
  const projectDir = path.resolve(options.project);
  const project = readProject(projectDir, descriptor.name);
  const config = resolveConfig(options, project.settings);
  if (project.settings.enabled === false) {
    reporter.warning(`The ${descriptor.name} target is turned off in this project (build.targets.${descriptor.name}.enabled); building it anyway`);
  }
  const physicsProblem = checkPhysics(project.modules, descriptor);
  const toolchains = checkedToolchains(descriptor, ctx);
  const report = doctorReport(descriptor, toolchains, ctx.platform, physicsProblem);
  if (!report.ok || options.json) reporter.doctor(report);
  for (const check of toolchains.checks) {
    if (!check.ok && !check.required) reporter.warning(`${check.title}: ${check.detail}. ${check.fix ?? ''}`.trim());
  }
  if (!report.ok && !options.dryRun) {
    throw new BuildError(physicsProblem ?? `This machine cannot build the ${descriptor.name} target yet (see above, or run n2build doctor --target ${descriptor.name})`);
  }

  const packFormat = resolvePackFormat(descriptor, project.settings, options.pack);
  const sevenZip = packFormat === '7z' ? findSevenZip(toolchains.env ?? ctx.env, ctx) : null;
  if (packFormat === '7z' && descriptor.package.kind === 'web') {
    throw new BuildError('The web player reads zip packs only (stored or zip): build the web target with --pack stored or zip');
  }
  if (packFormat === '7z' && !sevenZip && !options.dryRun) {
    throw new BuildError("A 7z pack needs 7-Zip's command line (7z, 7za or 7zz on PATH): install 7-Zip, or build with --pack zip");
  }
  if (buildsRemotely(descriptor, ctx.platform)) {
    return buildRemote({ descriptor, options, ctx, reporter, projectDir, project, config, toolchains, ready: report.ok });
  }
  if (descriptor.package.kind === 'android') {
    return buildAndroid({ descriptor, options, ctx, reporter, project, projectDir, config, toolchains, packFormat, sevenZip, ready: report.ok });
  }
  if (descriptor.package.kind === 'ios' && packFormat === 'folder') throw new BuildError('An iOS app holds the game as one pack: build with --pack zip, stored or 7z');
  // iOS: the pack and the app's identity are configure options (players/ios/CMakeLists.txt)
  const treeForPack = (plan) => packPath({ descriptor, packageDir: plan.packageDir, treeDir: plan.treeDir });
  const planOptions = {
    engineRoot: ENGINE_ROOT, projectDir, descriptor, config: config, modules: project.modules,
    present: toolchains.present, toolchainFile: toolchains.toolchainFile ?? (options.dryRun ? '<Emscripten.cmake from emsdk>' : null),
    buildRoot: options.buildDir ? path.resolve(options.buildDir) : null, outDir: options.out,
    jobs: options.jobs ?? os.availableParallelism(), gameName: project.name, packFormat,
  };
  let plan = planBuild(planOptions);
  if (descriptor.package.kind === 'ios') {
    plan = planBuild({ ...planOptions, extraConfigureArgs: iosConfigureArgs({ settings: project.settings, gameName: project.name, packFile: treeForPack(plan) }) });
  }
  const gamePack = packFormat === 'folder' ? null : packPath({ descriptor, packageDir: plan.packageDir, treeDir: plan.treeDir });
  if (options.dryRun) {
    reporter.step('plan', `Plan for ${descriptor.name} (${config})`);
    reporter.log('stdout', `build tree: ${plan.treeDir}`);
    reporter.log('stdout', `cmake ${plan.configureArgs.join(' ')}`);
    reporter.log('stdout', `cmake ${plan.buildArgs.join(' ')}`);
    reporter.log('stdout', `package: ${plan.packageDir}`);
    reporter.log('stdout', `game: ${gamePack ? `${packFormat} pack ${gamePack}` : 'the project folder, copied'}`);
    return { ok: report.ok, dryRun: true, output: plan.packageDir, treeDir: plan.treeDir, ...(report.ok ? {} : { error: 'Not ready (see the doctor report)' }) };
  }

  const cmake = findOnPath('cmake', toolchains.env, ctx) ?? 'cmake';
  // The web package preloads the pack (N2ENGINE_WEB_PACK) and the iOS app bundles it, so it is cooked before CMake runs
  if (packBeforeConfigure(descriptor) && gamePack) {
    reporter.step('cook', `Packing the game (${packFormat}) into ${gamePack}`);
    fs.mkdirSync(plan.treeDir, { recursive: true });
    cookPack({ projectDir, outFile: gamePack, format: packFormat, target: descriptor.name, game: project.name, sevenZip, env: toolchains.env });
  }
  if (needsConfigure(plan.treeDir, plan.configureArgs)) {
    reporter.step('configure', `Configuring ${descriptor.displayName} (${config}) in ${plan.treeDir}`);
    const code = await runProcess(cmake, plan.configureArgs, { env: toolchains.env, cwd: ENGINE_ROOT, reporter });
    if (code !== 0) throw new BuildError(`CMake could not configure the ${descriptor.name} build (exit code ${code}; see the log above)`);
    recordConfigure(plan.treeDir, plan.configureArgs);
  }
  reporter.step('compile', `Building ${descriptor.cmake.target}`);
  const code = await runProcess(cmake, plan.buildArgs, { env: toolchains.env, cwd: ENGINE_ROOT, reporter });
  if (code !== 0) throw new BuildError(`The ${descriptor.name} build failed (exit code ${code}; see the log above)`);

  reporter.step('package', `Packaging into ${plan.packageDir}`);
  preparePackageDir(plan.packageDir);
  const entry = descriptor.package.kind === 'desktop'
    ? packageDesktop({ treeDir: plan.treeDir, descriptor, projectDir, packageDir: plan.packageDir, gameName: project.name, platform: ctx.platform,
      pack: { format: packFormat, sevenZip, env: toolchains.env },
      physicsGpu: project.project.settings?.physics?.gpu,
      onLeftOut: (left) => reporter.log('stdout', `Not packaged: ${left.join(', ')} (the project's physics.gpu is off; set it to on or auto for GPU physics)`) })
    : descriptor.package.kind === 'ios'
      ? packageIos({ treeDir: plan.treeDir, config, packageDir: plan.packageDir, gameName: project.name })
      : packageWeb({ treeDir: plan.treeDir, descriptor, packageDir: plan.packageDir });
  writeMarker(plan.packageDir, {
    ...markerInfo({ descriptor, config, entry, project, engine: engineRevision(ENGINE_ROOT), builtAt: new Date().toISOString() }),
    ...(descriptor.package.kind === 'ios' ? { appId: iosSettings(project.settings, project.name).appId, sdk: 'iphonesimulator' } : {}),
  });
  let archive;
  if (options.archive) {
    reporter.step('archive', `Writing a .${options.archive} archive`);
    archive = archivePackage({ packageDir: plan.packageDir, format: options.archive, cmake, env: toolchains.env });
  }
  return { ok: true, output: plan.packageDir, entry, ...(archive ? { archive } : {}) };
}

/**
 * A build in GitHub Actions (docs/apple.html#remote): the game's repository must be pushed; the workflow runs n2build on a
 * macOS runner and uploads the package, which lands in the package folder here
 */
async function buildRemote({ descriptor, options, ctx, reporter, projectDir, project, config, toolchains, ready }) {
  const runGit = (args) => ctx.run('git', args, { env: ctx.env });
  const state = gitState(projectDir, runGit);
  const engineRef = engineRevision(ENGINE_ROOT);
  const packageDir = options.out ? path.resolve(options.out) : path.join(projectDir, '.n2', 'out', `${descriptor.name}-${config}`, safeName(project.name));
  if (options.dryRun) {
    reporter.step('plan', `Plan for ${descriptor.name} (${config}) in GitHub Actions`);
    reporter.log('stdout', `gh workflow run ${descriptor.remote.workflow} --ref ${state.branch} ${workflowInputs({ target: descriptor.name, config, projectPath: state.projectPath, engineRef }).join(' ')}`);
    reporter.log('stdout', `artifact: ${descriptor.remote.artifact}`);
    reporter.log('stdout', `package: ${packageDir}`);
    return { ok: ready, dryRun: true, remote: true, output: packageDir, ...(ready ? {} : { error: 'Not ready (see the doctor report)' }) };
  }
  checkPushed(state);
  preparePackageDir(packageDir);
  const gh = toolchains.info.gh ?? 'gh';
  const followed = await dispatchAndFollow({
    descriptor, state, config, engineRef, packageDir, reporter,
    ...(ctx.remote ?? {
      exec: (args) => captureProcess(gh, args, { env: ctx.env }),
      stream: (args, onLine) => runProcess(gh, args, { env: ctx.env, cwd: state.root, reporter: { log: (_s, line) => onLine(line) } }),
      sleep: (ms) => new Promise((resolve) => { setTimeout(resolve, ms); }),
    }),
  });
  // The workflow's own n2build wrote the marker; it is checked like any package
  const marker = readMarker(packageDir);
  if (marker.target !== descriptor.name) throw new BuildError(`The workflow's artifact holds a ${marker.target} package, not ${descriptor.name}`);
  return { ok: true, output: packageDir, entry: marker.entry, remote: followed.url };
}

/**
 * The Android build (docs/android.html): cook the pack into the tree's assets, copy the Gradle template in, and let Gradle
 * build libn2game.so (its CMake step, with the NDK) and the APK
 */
async function buildAndroid({ descriptor, options, ctx, reporter, project, projectDir, config, toolchains, packFormat, sevenZip, ready }) {
  if (packFormat === 'folder') throw new BuildError('An APK holds the game as one pack: build the android target with --pack zip, stored or 7z');
  const cmake = findOnPath('cmake', toolchains.env, ctx);
  // The signing profile: named by --signing or the project, resolved from the user's n2build.json (lib/signing.mjs). A
  // debug build always uses the SDK's debug key
  const profileName = signingProfileName(options, project.settings);
  if (profileName && config === 'debug') reporter.warning(`A debug build is signed with the SDK's debug key; the signing profile "${profileName}" is used for release builds`);
  const plan = planAndroid({
    engineRoot: ENGINE_ROOT, projectDir, descriptor, config, modules: project.modules, settings: project.settings,
    info: toolchains.info, cmakeDir: cmakeInstallDir(cmake), buildRoot: options.buildDir ? path.resolve(options.buildDir) : null,
    outDir: options.out, gameName: project.name, signingProfile: profileName,
  });
  const gradle = toolchains.info.gradle ?? 'gradle';
  if (options.dryRun) {
    reporter.step('plan', `Plan for ${descriptor.name} (${config})`);
    reporter.log('stdout', `build tree: ${plan.treeDir}`);
    reporter.log('stdout', `game: ${packFormat} pack ${plan.packFile}`);
    reporter.log('stdout', `gradle ${plan.gradleArgs.join(' ')}`);
    reporter.log('stdout', `apk: ${plan.apk}`);
    if (plan.aab) reporter.log('stdout', `aab: ${plan.aab}`);
    reporter.log('stdout', `signing: ${plan.signingProfile ? `the profile "${plan.signingProfile}" (read when the build runs)` : plan.signed ? 'the SDK debug key' : 'none (an unsigned APK)'}`);
    reporter.log('stdout', `package: ${plan.packageDir}`);
    return { ok: ready, dryRun: true, output: plan.packageDir, treeDir: plan.treeDir, ...(ready ? {} : { error: 'Not ready (see the doctor report)' }) };
  }
  // Read only now, when the build runs; its passwords are hidden in every line from here on, and reach Gradle only in its
  // process environment
  let gradleEnv = withoutSigningEnv(toolchains.env);
  if (plan.signingProfile) {
    const settingsFile = userSettingsFile(ctx.env, ctx.platform);
    const signing = resolveAndroidSigning(plan.signingProfile, readUserSettings(settingsFile), {
      env: ctx.env, settingsFile: settingsFile ?? 'n2build.json',
      ...(ctx.exists ? { exists: ctx.exists } : {}),
      ...(ctx.readFile ? { readFile: ctx.readFile } : {}),
    });
    reporter.addSecret(signing.storePassword);
    reporter.addSecret(signing.keyPassword);
    gradleEnv = { ...gradleEnv, ...androidSigningEnv(signing) };
  }
  reporter.step('cook', `Packing the game (${packFormat}) into ${plan.packFile}`);
  prepareGradleTree(plan);
  cookPack({ projectDir, outFile: plan.packFile, format: packFormat, target: descriptor.name, game: project.name, sevenZip, env: toolchains.env });

  reporter.step('compile', `Building the ${plan.aab ? 'APK and AAB' : 'APK'} with Gradle (${config}; the engine for ${plan.properties.abis})`);
  const invocation = gradleInvocation(gradle, plan.gradleArgs, ctx.platform);
  const code = await runProcess(invocation.command, invocation.args, { env: gradleEnv, cwd: plan.gradleDir, reporter, spawnOptions: invocation.options });
  if (code !== 0) throw new BuildError(`The ${descriptor.name} build failed (Gradle exit code ${code}; see the log above)`);

  reporter.step('package', `Packaging into ${plan.packageDir}`);
  preparePackageDir(plan.packageDir);
  const files = packageAndroid({ plan, packageDir: plan.packageDir, gameName: project.name });
  const entry = files.apk;
  // The profile's name only: never its keystore path or anything read from it
  writeMarker(plan.packageDir, {
    ...markerInfo({ descriptor, config, entry, project, engine: engineRevision(ENGINE_ROOT), builtAt: new Date().toISOString() }),
    appId: plan.appId, abis: plan.properties.abis.split(','), signed: plan.signed,
    ...(plan.signingProfile ? { signingProfile: plan.signingProfile } : {}), ...(files.aab ? { bundle: files.aab } : {}),
  });
  if (!plan.signed) {
    reporter.warning('The release APK is unsigned: Android installs only signed APKs. Name a signing profile (build.targets.android.signing, or --signing; docs/android.html#signing)');
  }
  let archive;
  if (options.archive) {
    reporter.step('archive', `Writing a .${options.archive} archive`);
    archive = archivePackage({ packageDir: plan.packageDir, format: options.archive, cmake: cmake ?? 'cmake', env: toolchains.env });
  }
  return { ok: true, output: plan.packageDir, entry, ...(files.aab ? { bundle: files.aab } : {}), ...(archive ? { archive } : {}) };
}

/** n2build cook: the game's pack alone (what build puts in a package), with its manifest beside it */
async function cook(descriptor, options, ctx, reporter) {
  const projectDir = path.resolve(options.project);
  const project = readProject(projectDir, descriptor.name);
  const format = resolvePackFormat(descriptor, project.settings, options.pack);
  if (format === 'folder') throw new BuildError(`The ${descriptor.name} target packages the project folder as it is; choose --pack zip, stored or 7z`);
  const outFile = path.resolve(options.out ?? path.join(projectDir, '.n2', 'out', `${descriptor.name}-game${PACK_EXTENSION}`));
  reporter.step('cook', `Packing the game (${format}) into ${outFile}`);
  const manifest = cookPack({ projectDir, outFile, format, target: descriptor.name, game: project.name, sevenZip: findSevenZip(ctx.env, ctx), env: ctx.env });
  return { ok: true, output: outFile, files: manifest.fileCount, size: fs.statSync(outFile).size, uncompressedSize: manifest.totalSize };
}

async function run(descriptor, options, ctx, reporter) {
  let packageDir;
  if (options.noBuild) {
    const projectDir = path.resolve(options.project);
    const project = readProject(projectDir, descriptor.name);
    const toolchains = { present: new Set(descriptor.toolchains.optional) };
    packageDir = options.out ? path.resolve(options.out) : planBuild({
      engineRoot: ENGINE_ROOT, projectDir, descriptor, config: resolveConfig(options, project.settings), modules: project.modules,
      present: toolchains.present, toolchainFile: '-', gameName: project.name,
    }).packageDir;
  } else {
    const built = await build(descriptor, { ...options, archive: undefined }, ctx, reporter);
    packageDir = built.output;
  }
  const marker = readMarker(packageDir);
  if (marker.target !== descriptor.name) throw new BuildError(`${packageDir} holds a ${marker.target} package, not ${descriptor.name}`);

  if (descriptor.run.kind === 'executable') {
    const executable = path.join(packageDir, marker.entry);
    const args = ['--console-log', ...(options.frames ? ['--frames', String(options.frames)] : [])];
    reporter.step('run', `Running ${executable}`);
    const code = await runProcess(executable, args, { env: ctx.env, cwd: packageDir, reporter });
    if (code !== 0) throw new BuildError(`The game exited with code ${code}`);
    return { ok: true, output: packageDir };
  }

  if (descriptor.run.kind === 'ios-simulator') {
    if (ctx.platform !== 'darwin') {
      throw new BuildError('The iOS simulator runs on a Mac only. Off a Mac, the n2build-apple workflow runs the app in the simulator for 30 seconds (docs/apple.html#remote)');
    }
    const outcome = await runInSimulator({ app: path.join(packageDir, marker.entry), bundleId: marker.appId, env: ctx.env, reporter,
      seconds: options.seconds ?? null, ...(ctx.simulator ?? { exec: captureProcess }) });
    return { ok: true, output: packageDir, device: outcome.device, ended: outcome.reason };
  }

  if (descriptor.run.kind === 'android-device') {
    if (!marker.signed) throw new BuildError(`${marker.entry} is unsigned, and Android installs only signed APKs: build debug, or name a signing profile`);
    const sdk = checkToolchains({ ...descriptor, toolchains: { required: ['android-sdk'], optional: [] } }, ctx).info.androidSdk;
    const adb = findAdb(sdk, ctx);
    if (!adb) throw new BuildError("adb was not found (the Android SDK's platform-tools): install them with the SDK Manager");
    const listed = ctx.run(adb, ['devices', '-l'], { env: ctx.env });
    if (!listed || listed.status !== 0) throw new BuildError(`${adb} devices failed: ${listed?.stderr?.trim() || 'it did not start'}`);
    const device = pickDevice(parseDevices(listed.stdout), options.device);
    const outcome = await runOnDevice({ adb, apk: path.join(packageDir, marker.entry), appId: marker.appId, device, env: ctx.env, reporter,
      seconds: options.seconds ?? null, ...(ctx.adb ?? {}) });
    return { ok: true, output: packageDir, device: device.serial, pid: outcome.pid, ended: outcome.reason };
  }

  if (options.frames) {
    // Headless: the web package smoke test runs the package under node for N frames (scripts/web-package-smoke.mjs)
    reporter.step('run', `Running ${packageDir} under node for ${options.frames} frames`);
    const smoke = path.join(ENGINE_ROOT, 'scripts', 'web-package-smoke.mjs');
    // The size and speed profiles add .br/.gz copies, which the smoke test checks only when told to expect them
    const precompressed = fs.existsSync(path.join(packageDir, 'index.wasm.br')) ? ['--precompressed'] : [];
    const code = await runProcess(process.execPath, [smoke, packageDir, String(options.frames), '320x180', ...precompressed], { env: ctx.env, cwd: ENGINE_ROOT, reporter });
    if (code !== 0) throw new BuildError(`The web package smoke run failed (exit code ${code})`);
    return { ok: true, output: packageDir };
  }
  const { url } = await serveFolder(packageDir, options.port ?? 8080);
  reporter.step('serve', `Serving ${packageDir}; open ${url} in a browser, Ctrl+C to stop`);
  reporter.result({ ok: true, output: packageDir, url, serving: true });
  await new Promise(() => {});
  return null;
}

export async function main(argv, { ctx = systemProbe(), reporter } = {}) {
  let parsed;
  try {
    parsed = parseArguments(argv);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    const r = reporter ?? new Reporter({ json: argv.includes('--json') });
    r.result({ ok: false, error: `${error.message}\n${USAGE}` });
    return 2;
  }
  const { command, options } = parsed;
  const r = reporter ?? new Reporter({ json: Boolean(options.json) });
  if (command === 'help' || options.help) {
    r.out(USAGE);
    return 0;
  }
  try {
    const descriptors = loadDescriptors(ENGINE_ROOT, extraTargetFolders(options, ctx.env, ctx.platform));
    if (command === 'list') {
      r.targets([...descriptors.values()].map((d) => ({
        name: d.name, displayName: d.displayName, hostSystems: d.hostSystems, buildableHere: d.hostSystems.includes(ctx.platform) || buildsRemotely(d, ctx.platform),
        ...(buildsRemotely(d, ctx.platform) ? { remote: d.remote.kind } : {}), source: d.source,
      })));
      r.result({ ok: true });
      return 0;
    }
    if (command === 'apple-workflow') {
      const state = gitState(path.resolve(options.project), (args) => ctx.run('git', args, { env: ctx.env }));
      const file = path.join(state.root, '.github', 'workflows', 'n2build-apple.yml');
      if (fs.existsSync(file) && !fs.readFileSync(file, 'utf8').startsWith('# Written by n2build apple-workflow')) {
        throw new BuildError(`${file} exists and was not written by n2build; move it away first`);
      }
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, workflowTemplate());
      r.result({ ok: true, output: file, next: 'Commit and push it; then build --target ios or macos' });
      return 0;
    }
    const descriptor = findTarget(descriptors, options.target);
    if (command === 'doctor') {
      let physicsProblem;
      if (options.project) physicsProblem = checkPhysics(readProject(path.resolve(options.project), descriptor.name).modules, descriptor);
      const report = doctorReport(descriptor, checkedToolchains(descriptor, ctx), ctx.platform, physicsProblem);
      r.doctor(report);
      r.result({ ok: report.ok, ...(report.ok ? {} : { error: `Not ready to build ${descriptor.name}` }) });
      return report.ok ? 0 : 1;
    }
    const result = command === 'build' ? await build(descriptor, options, ctx, r)
      : command === 'cook' ? await cook(descriptor, options, ctx, r)
        : await run(descriptor, options, ctx, r);
    if (result) r.result(result);
    return result && !result.ok ? 1 : 0;
  } catch (error) {
    if (!(error instanceof BuildError)) throw error;
    r.result({ ok: false, error: error.message });
    return 1;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  process.exitCode = await main(process.argv.slice(2));
}
