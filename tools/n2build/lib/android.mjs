// The Android build (build targets stage 8; docs/android.html). Gradle builds the APK from players/android, a template
// that n2build copies into the build tree; Gradle's CMake step builds libn2game.so from the engine repository with the
// NDK. Every game setting goes to Gradle as a property (-Pn2.<name>=...), so the template holds nothing of a game's.
//
//   planAndroid            every path, property and command, decided before anything runs (--dry-run prints it)
//   prepareGradleTree      the template copied into the tree, and local.properties (the SDK, and a CMake 3.25 or newer)
//   gradleInvocation       how to start Gradle (gradle.bat on Windows goes through cmd.exe, with checked arguments)
//   packageAndroid         the APK (and a release AAB) into the package folder, renamed after the game
//
// Signing (stage 9; lib/signing.mjs): a debug APK is signed with the SDK's debug key. A release build with a signing profile
// is signed with it and also gives an AAB (the format Google Play takes); without one, the release APK is left unsigned.
import fs from 'node:fs';
import path from 'node:path';

import { buildKey, safeName } from './build.mjs';
import { PACK_EXTENSION } from './cook.mjs';
import { BuildError } from './report.mjs';
import { cmdSafe, commandInvocation } from './toolchains.mjs';

export const ANDROID_ABIS = ['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'];
export const DEFAULT_ABIS = ['arm64-v8a', 'x86_64'];
export const ORIENTATIONS = { any: 'fullUser', landscape: 'sensorLandscape', portrait: 'sensorPortrait' };
const APP_ID_SEGMENT = /^[A-Za-z][A-Za-z0-9_]*$/;
// Java's reserved words cannot be a segment of a package name
const JAVA_KEYWORDS = new Set(['abstract', 'assert', 'boolean', 'break', 'byte', 'case', 'catch', 'char', 'class', 'const', 'continue',
  'default', 'do', 'double', 'else', 'enum', 'extends', 'false', 'final', 'finally', 'float', 'for', 'goto', 'if', 'implements',
  'import', 'instanceof', 'int', 'interface', 'long', 'native', 'new', 'null', 'package', 'private', 'protected', 'public',
  'return', 'short', 'static', 'strictfp', 'super', 'switch', 'synchronized', 'this', 'throw', 'throws', 'transient', 'true',
  'try', 'void', 'volatile', 'while']);

/** The rule IO::BuildTargets applies to appId, plus Android's: no Java keyword as a segment */
export function isAppId(text) {
  if (typeof text !== 'string' || text.length < 3 || text.length > 150) return false;
  const segments = text.split('.');
  return segments.length >= 2 && segments.every((s) => APP_ID_SEGMENT.test(s) && !JAVA_KEYWORDS.has(s));
}

/** An app id made from the game's name when the project names none: com.n2engine.<name> */
export function defaultAppId(gameName) {
  let segment = (gameName ?? '').toLowerCase().replace(/[^a-z0-9_]/g, '');
  if (!/^[a-z]/.test(segment)) segment = `game${segment}`;
  if (JAVA_KEYWORDS.has(segment)) segment = `${segment}_`;
  return `com.n2engine.${segment.slice(0, 100)}`;
}

/** build.targets.android's Android keys (appId, orientation, abis), checked; throws a BuildError naming the key */
export function androidSettings(settings, gameName) {
  const where = 'build.targets.android';
  const appId = settings.appId ?? defaultAppId(gameName);
  if (!isAppId(appId)) {
    throw new BuildError(`${where}.appId must be a reverse-domain id such as com.example.game (two or more segments of letters, digits and _, each starting with a letter, no Java keyword)`);
  }
  const orientation = settings.orientation ?? 'any';
  if (!Object.hasOwn(ORIENTATIONS, orientation)) throw new BuildError(`${where}.orientation must be "any", "landscape" or "portrait"`);
  const abis = settings.abis ?? DEFAULT_ABIS;
  if (!Array.isArray(abis) || abis.length === 0 || abis.some((a) => !ANDROID_ABIS.includes(a)) || new Set(abis).size !== abis.length) {
    throw new BuildError(`${where}.abis must list one or more of ${ANDROID_ABIS.join(', ')}, each once`);
  }
  return { appId, orientation, abis };
}

/** A Gradle property value: one line, no characters the Windows command line would read (gradleInvocation) */
function checkPropertyValue(name, value) {
  if (!cmdSafe(value)) {
    throw new BuildError(`${name} (${value}) holds a character n2build cannot pass to Gradle safely ("%!^&|<> or a control character); rename or move it`);
  }
  return value;
}

/** local.properties' path form: forward slashes, with ':' and '\' escaped as a Java properties file wants */
export function propertiesPath(file) {
  return file.replace(/\\/g, '/').replace(/:/g, '\\:');
}

/**
 * Every path and argument of an Android build. info is checkToolchains' (androidSdk, androidNdk, gradle); cmakeDir is
 * the folder holding bin/cmake of a CMake 3.25 or newer (the SDK's own CMake 3.22 is too old for the engine).
 */
export function planAndroid({ engineRoot, projectDir, descriptor, config, modules, settings, info, cmakeDir, buildRoot, outDir, gameName, signingProfile = null }) {
  if (!['debug', 'release'].includes(config)) throw new BuildError(`The android target builds debug or release, not '${config}'`);
  const android = androidSettings(settings ?? {}, gameName);
  const options = { ...descriptor.cmake.options, ...descriptor.cmake.configs[config] };
  const key = buildKey({ projectDir, target: descriptor.name, config, modules, options: { ...options, abis: android.abis } });
  const treeDir = path.join(buildRoot ?? path.join(projectDir, '.n2', 'build'), key);
  const gradleDir = path.join(treeDir, 'gradle');
  const assetsDir = path.join(treeDir, 'assets');
  const packageDir = outDir ? path.resolve(outDir) : path.join(projectDir, '.n2', 'out', `${descriptor.name}-${config}`, safeName(gameName));
  const variant = config === 'debug' ? 'Debug' : 'Release';
  const properties = {
    engineRoot: path.resolve(engineRoot),
    project: path.resolve(projectDir),
    appId: android.appId,
    displayName: settings?.displayName ?? (gameName || 'N2Engine Game'),
    version: settings?.version ?? '1.0.0',
    buildNumber: String(settings?.buildNumber ?? 1),
    orientation: ORIENTATIONS[android.orientation],
    abis: android.abis.join(','),
    assetsDir,
    nativeBuildDir: path.join(treeDir, 'native'),
    ndkVersion: info?.androidNdk ?? descriptor.pins?.['android-ndk'] ?? '',
    cmakeBuildType: options.CMAKE_BUILD_TYPE ?? (config === 'debug' ? 'Debug' : 'Release'),
  };
  for (const [name, value] of Object.entries(properties)) checkPropertyValue(`n2.${name}`, value);
  // A release build signed with a profile is the one to publish: Gradle builds the AAB too
  const profileSigned = config === 'release' && signingProfile !== null;
  const tasks = [`assemble${variant}`, ...(profileSigned ? ['bundleRelease'] : [])];
  const gradleArgs = ['-p', gradleDir, ...tasks, '--no-daemon', '--console=plain',
    ...Object.entries(properties).map(([name, value]) => `-Pn2.${name}=${value}`)];
  const apkName = config === 'debug' ? 'app-debug.apk' : profileSigned ? 'app-release.apk' : 'app-release-unsigned.apk';
  const apk = path.join(gradleDir, 'app', 'build', 'outputs', 'apk', config, apkName);
  const aab = profileSigned ? path.join(gradleDir, 'app', 'build', 'outputs', 'bundle', 'release', 'app-release.aab') : null;
  const localProperties = [
    '# Written by n2build for this build tree',
    ...(info?.androidSdk ? [`sdk.dir=${propertiesPath(info.androidSdk)}`] : []),
    ...(cmakeDir ? [`cmake.dir=${propertiesPath(cmakeDir)}`] : []),
    '',
  ].join('\n');
  return {
    key, treeDir, gradleDir, assetsDir, packageDir, packFile: path.join(assetsDir, `game${PACK_EXTENSION}`),
    properties, gradleArgs, tasks, apk, aab, signed: config === 'debug' || profileSigned, signingProfile: profileSigned ? signingProfile : null,
    localProperties, appId: android.appId,
    template: path.join(engineRoot, descriptor.package.from),
  };
}

/** The CMake install folder (the one holding bin/) for a cmake executable path */
export function cmakeInstallDir(cmakeExe) {
  return cmakeExe ? path.dirname(path.dirname(cmakeExe)) : null;
}

/**
 * The template into <tree>/gradle. Copied over what is there (not emptied), so Gradle's own build/ folders stay and a
 * second build is incremental; local.properties is rewritten each time.
 */
export function prepareGradleTree(plan) {
  fs.mkdirSync(plan.gradleDir, { recursive: true });
  fs.cpSync(plan.template, plan.gradleDir, {
    recursive: true,
    force: true,
    // The template's own Gradle output, if someone opened it in Android Studio, is not part of it
    filter: (source) => {
      const rel = path.relative(plan.template, source).split(path.sep);
      return !rel.includes('build') && !rel.includes('.gradle') && !rel.includes('.cxx') && rel[0] !== 'native' && rel[rel.length - 1] !== 'local.properties';
    },
  });
  fs.writeFileSync(path.join(plan.gradleDir, 'local.properties'), plan.localProperties);
  fs.mkdirSync(plan.assetsDir, { recursive: true });
}

/**
 * How to start Gradle. On Windows gradle is gradle.bat, which Node starts only through cmd.exe (commandInvocation quotes
 * the line and refuses a character cmd.exe would read; planAndroid has already checked the values).
 */
export function gradleInvocation(gradle, args, platform = process.platform) {
  const invocation = commandInvocation(gradle, args, platform);
  if (!invocation) throw new BuildError(`${gradle}: a path or argument holds a character n2build cannot pass to cmd.exe ("%!^&|<>)`);
  return invocation;
}

/** The APK into the package folder as <Game>.apk, and a signed release's AAB as <Game>.aab; returns their file names */
export function packageAndroid({ plan, packageDir, gameName }) {
  if (!fs.existsSync(plan.apk)) throw new BuildError(`${plan.apk} is missing: Gradle did not produce the APK`);
  const name = `${safeName(gameName)}${plan.signed ? '' : '-unsigned'}.apk`;
  fs.copyFileSync(plan.apk, path.join(packageDir, name));
  let bundle = null;
  if (plan.aab) {
    if (!fs.existsSync(plan.aab)) throw new BuildError(`${plan.aab} is missing: Gradle did not produce the AAB`);
    bundle = `${safeName(gameName)}.aab`;
    fs.copyFileSync(plan.aab, path.join(packageDir, bundle));
  }
  return { apk: name, aab: bundle };
}
