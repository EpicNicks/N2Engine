// Signing profiles (build targets stage 9; docs/android.html#signing). A project names a profile, never its contents:
// build.targets.<target>.signing = {"profile": "<name>"} (IO::BuildTargets refuses anything else there). The profile itself
// lives in the user's own n2build.json, outside every project:
//
//   "signingProfiles": {
//     "release": {
//       "android": {
//         "keystore": "C:/Users/me/keys/game.jks",
//         "keyAlias": "upload",
//         "storePassword": { "env": "GAME_STORE_PASSWORD" },     or { "file": "C:/Users/me/keys/store.pass" }, or "text"
//         "keyPassword":   { "file": "C:/Users/me/keys/key.pass" }  (absent: the store password)
//       }
//     }
//   }
//
// It is resolved only when a build runs. The passwords go to Gradle in the environment of its process (never on its
// command line, in a file in the build tree, in the project or over the editor protocol), and the reporter hides them in
// every line it prints (Reporter.addSecret).
import fs from 'node:fs';
import path from 'node:path';

import { BuildError } from './report.mjs';

const PROFILE_NAME = /^[A-Za-z0-9._-]{1,64}$/;
const ENV_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

/** The environment variables the Android Gradle template reads (players/android/app/build.gradle, signingConfigs.n2) */
export const ANDROID_SIGNING_ENV = {
  storeFile: 'N2_SIGNING_STORE_FILE',
  storePassword: 'N2_SIGNING_STORE_PASSWORD',
  keyAlias: 'N2_SIGNING_KEY_ALIAS',
  keyPassword: 'N2_SIGNING_KEY_PASSWORD',
};

/** The profile name a build uses: --signing, else build.targets.<target>.signing.profile, else none */
export function signingProfileName(options, settings) {
  const name = options.signing ?? settings?.signing?.profile;
  if (name === undefined) return null;
  if (typeof name !== 'string' || !PROFILE_NAME.test(name)) {
    throw new BuildError('A signing profile name is letters, digits, ".", "_" and "-", 1 to 64 characters');
  }
  return name;
}

/** A password from its source: a string, {"env": NAME} or {"file": PATH} (the file's first line) */
function readPassword(source, where, { env, readFile }) {
  if (typeof source === 'string') {
    if (source === '') throw new BuildError(`${where} is empty`);
    return source;
  }
  if (source && typeof source === 'object' && !Array.isArray(source)) {
    const keys = Object.keys(source);
    if (keys.length === 1 && keys[0] === 'env') {
      if (typeof source.env !== 'string' || !ENV_NAME.test(source.env)) throw new BuildError(`${where}.env must be an environment variable name`);
      const value = env[source.env];
      if (value === undefined || value === '') throw new BuildError(`${where}: the environment variable ${source.env} is not set`);
      return value;
    }
    if (keys.length === 1 && keys[0] === 'file') {
      if (typeof source.file !== 'string' || !path.isAbsolute(source.file)) throw new BuildError(`${where}.file must be an absolute path`);
      const text = readFile(source.file);
      if (text === null) throw new BuildError(`${where}: ${source.file} cannot be read`);
      const value = text.split(/\r?\n/)[0];
      if (value === '') throw new BuildError(`${where}: ${source.file} is empty`);
      return value;
    }
  }
  throw new BuildError(`${where} must be a string, {"env": "NAME"} or {"file": "/absolute/path"}`);
}

const defaultReadFile = (file) => {
  try {
    return fs.readFileSync(file, 'utf8');
  } catch {
    return null;
  }
};

/**
 * The Android part of a profile, with its passwords read. Throws a BuildError that names the profile and the key, never a
 * password. userSettings is n2build.json, parsed (readUserSettings); settingsFile names it in the messages.
 * @returns {{profile: string, storeFile: string, keyAlias: string, storePassword: string, keyPassword: string}}
 */
export function resolveAndroidSigning(name, userSettings, { env = process.env, exists = fs.existsSync, readFile = defaultReadFile, settingsFile = 'n2build.json' } = {}) {
  const profiles = userSettings?.signingProfiles;
  const fix = `add it under "signingProfiles" in ${settingsFile} (docs/android.html#signing)`;
  if (profiles === undefined || profiles === null || typeof profiles !== 'object' || Array.isArray(profiles) || !Object.hasOwn(profiles, name)) {
    throw new BuildError(`The project asks for the signing profile "${name}", which is not in your settings: ${fix}`);
  }
  const android = profiles[name]?.android;
  const where = `signingProfiles.${name}.android`;
  if (!android || typeof android !== 'object' || Array.isArray(android)) {
    throw new BuildError(`The signing profile "${name}" has no "android" part: ${fix}`);
  }
  for (const key of Object.keys(android)) {
    if (!['keystore', 'keyAlias', 'storePassword', 'keyPassword'].includes(key)) throw new BuildError(`${where}: unknown key "${key}"`);
  }
  if (typeof android.keystore !== 'string' || !path.isAbsolute(android.keystore)) throw new BuildError(`${where}.keystore must be an absolute path to the keystore`);
  if (!exists(android.keystore)) throw new BuildError(`${where}.keystore: ${android.keystore} does not exist`);
  if (typeof android.keyAlias !== 'string' || android.keyAlias === '') throw new BuildError(`${where}.keyAlias must name the key in the keystore`);
  const io = { env, readFile };
  const storePassword = readPassword(android.storePassword, `${where}.storePassword`, io);
  const keyPassword = android.keyPassword === undefined ? storePassword : readPassword(android.keyPassword, `${where}.keyPassword`, io);
  return { profile: name, storeFile: android.keystore, keyAlias: android.keyAlias, storePassword, keyPassword };
}

/** The environment additions for Gradle: the profile, by the names the template reads */
export function androidSigningEnv(signing) {
  return {
    [ANDROID_SIGNING_ENV.storeFile]: signing.storeFile,
    [ANDROID_SIGNING_ENV.storePassword]: signing.storePassword,
    [ANDROID_SIGNING_ENV.keyAlias]: signing.keyAlias,
    [ANDROID_SIGNING_ENV.keyPassword]: signing.keyPassword,
  };
}

/** The environment without any signing variable: the debug build, and every tool but Gradle, never sees a profile */
export function withoutSigningEnv(env) {
  const next = { ...env };
  for (const name of Object.values(ANDROID_SIGNING_ENV)) delete next[name];
  return next;
}
