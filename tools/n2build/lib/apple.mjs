// Apple targets on a Mac (build targets stage 10; docs/apple.html). macOS is a desktop target (lib/build.mjs packages it);
// iOS is an app bundle built with CMake's Xcode generator from players/ios:
//
//   iosSettings          build.targets.ios's keys (appId, orientation), checked
//   iosConfigureArgs     the -D options that put the game's pack and settings into the app
//   findAppBundle        N2Game.app in the build tree (Xcode puts it in a folder per configuration and SDK)
//   packageIos           the app into the package folder, renamed after the game
//   runInSimulator       install in a booted (or booted here) simulator, launch, stream its console as log events
//
// Off a Mac the targets build in GitHub Actions instead (lib/remote.mjs, decision D7).
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { isAppId, defaultAppId } from './android.mjs';
import { safeName } from './build.mjs';
import { BuildError } from './report.mjs';

export const IOS_APP_NAME = 'N2Game.app';
const ORIENTATIONS = ['any', 'landscape', 'portrait'];

/** build.targets.ios: appId (com.n2engine.<name> by default) and orientation; throws a BuildError naming the key */
export function iosSettings(settings, gameName) {
  const appId = settings?.appId ?? defaultAppId(gameName);
  if (!isAppId(appId)) throw new BuildError('build.targets.ios.appId must be a reverse-domain id such as com.example.game');
  const orientation = settings?.orientation ?? 'any';
  if (!ORIENTATIONS.includes(orientation)) throw new BuildError('build.targets.ios.orientation must be "any", "landscape" or "portrait"');
  return { appId, orientation };
}

/** The configure options for players/ios: the pack and the game's settings (players/ios/CMakeLists.txt) */
export function iosConfigureArgs({ settings, gameName, packFile }) {
  const ios = iosSettings(settings, gameName);
  return [
    `-DN2ENGINE_IOS_PACK=${packFile}`,
    `-DN2ENGINE_IOS_BUNDLE_ID=${ios.appId}`,
    `-DN2ENGINE_IOS_NAME=${settings?.displayName ?? (gameName || 'N2Engine Game')}`,
    `-DN2ENGINE_IOS_VERSION=${settings?.version ?? '1.0.0'}`,
    `-DN2ENGINE_IOS_BUILD=${settings?.buildNumber ?? 1}`,
    `-DN2ENGINE_IOS_ORIENTATION=${ios.orientation}`,
  ];
}

/** The app bundle Xcode built, searched for under the tree's players/ios (a folder per configuration and SDK) */
export function findAppBundle(treeDir, config) {
  const wanted = config === 'debug' ? 'Debug' : 'Release';
  const found = [];
  const walk = (dir, depth) => {
    if (depth > 4 || !fs.existsSync(dir)) return;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (!entry.isDirectory()) continue;
      const full = path.join(dir, entry.name);
      if (entry.name === IOS_APP_NAME) found.push(full);
      else if (!entry.name.endsWith('.build') && entry.name !== 'CMakeFiles') walk(full, depth + 1);
    }
  };
  walk(treeDir, 0);
  // The configuration's own folder (Release-iphonesimulator) when there are several
  return found.find((p) => path.basename(path.dirname(p)).startsWith(`${wanted}-`)) ?? found[0] ?? null;
}

/** The app into the package as <Game>.app; returns its folder name */
export function packageIos({ treeDir, config, packageDir, gameName }) {
  const app = findAppBundle(treeDir, config);
  if (!app) throw new BuildError(`No ${IOS_APP_NAME} under ${treeDir}: the build did not produce the app`);
  const name = `${safeName(gameName)}.app`;
  fs.cpSync(app, path.join(packageDir, name), { recursive: true, verbatimSymlinks: true });
  return name;
}

/** `xcrun simctl list devices available --json` -> the booted device, else the newest iPhone; null when there is none */
export function pickSimulator(listJson) {
  let list;
  try {
    list = JSON.parse(listJson);
  } catch {
    return null;
  }
  const devices = [];
  for (const [runtime, entries] of Object.entries(list?.devices ?? {})) {
    if (!/iOS/.test(runtime)) continue;
    for (const d of entries ?? []) devices.push({ ...d, runtime });
  }
  const booted = devices.find((d) => d.state === 'Booted');
  if (booted) return { udid: booted.udid, name: booted.name, booted: true };
  const iphones = devices.filter((d) => /^iPhone/.test(d.name) && d.isAvailable !== false);
  // The newest runtime's (its key ends in the version: ...SimRuntime.iOS-18-2), then the last listed model
  iphones.sort((a, b) => a.runtime.localeCompare(b.runtime, undefined, { numeric: true }));
  const chosen = iphones.at(-1);
  return chosen ? { udid: chosen.udid, name: chosen.name, booted: false } : null;
}

/**
 * Install and launch in the simulator, the game's log streamed as log events ("stream": "console") until the app exits or
 * `seconds` pass. The player writes its log to the system log too (os_log, players/ios/src/IosMain.mm), and that is
 * what is followed here: the simulator's `log stream` for the app's process, its "N2Engine" lines. Neither
 * `simctl launch --console-pty` nor `--stdout=<file>` delivered the output reliably on CI runners while the app ran.
 * Whether the app still runs: a simulator app is a process of this Mac, and simctl launch prints its pid (the simulator's
 * launchctl list does not list apps). `exec` (a whole-output runner), `stream` (lib/adb.mjs's streamLines shape), `isAlive`
 * (pid -> boolean) and `sleep` are injectable for tests.
 */
export async function runInSimulator({ app, bundleId, env, reporter, seconds = null, exec, stream, isAlive = processAlive,
  processName = IOS_APP_NAME.replace(/\.app$/, ''), sleep = (ms) => new Promise((r) => { setTimeout(r, ms); }) }) {
  const simctl = async (args, what) => {
    const result = await exec('xcrun', ['simctl', ...args], { env });
    if (result.code !== 0) throw new BuildError(`${what} failed: ${(result.stderr || result.stdout).trim().split(/\r?\n/).slice(-3).join(' ')}`);
    return result;
  };
  const device = pickSimulator((await simctl(['list', 'devices', 'available', '--json'], 'Listing the simulators')).stdout);
  if (!device) throw new BuildError('No iOS simulator is available: add one in Xcode (Window > Devices and Simulators)');
  if (!device.booted) {
    reporter.step('boot', `Booting the ${device.name} simulator`);
    await simctl(['boot', device.udid], 'Booting the simulator');
  }
  // simctl boot returns before the system has finished starting: bootstatus -b waits until the device is ready
  await simctl(['bootstatus', device.udid, '-b'], 'Waiting for the simulator to finish booting');
  reporter.step('install', `Installing ${path.basename(app)} on ${device.name}`);
  await simctl(['install', device.udid, app], 'Installing the app');

  // The log stream starts before the launch, so the first lines are not missed
  const log = stream('xcrun', ['simctl', 'spawn', device.udid, 'log', 'stream', '--style', 'compact', '--level', 'info',
    '--predicate', `process == "${processName}" AND eventMessage BEGINSWITH "N2Engine"`], {
    env,
    onLine: (line) => {
      const message = /\]\s+(N2Engine .*)$/.exec(line)?.[1];
      if (message) reporter.log('console', message);
    },
  });
  await sleep(1000);
  reporter.step('run', `Launching ${bundleId}; its log follows`);
  let launched;
  try {
    launched = await simctl(['launch', '--terminate-running-process', device.udid, bundleId], 'Launching the app');
  } catch (error) {
    log.stop();
    await log.done;
    throw error;
  }
  const pid = /:\s*(\d+)/.exec(launched.stdout)?.[1] ?? null;

  let waited = 0;
  let reason = 'time';
  while (seconds === null || waited < seconds * 1000) {
    await sleep(500);
    waited += 500;
    // Without a pid (simctl printed none) the run lasts until --seconds
    if (waited % 2000 === 0 && pid !== null && !isAlive(Number(pid))) {
      reason = 'exited';
      break;
    }
  }
  if (reason === 'time') await exec('xcrun', ['simctl', 'terminate', device.udid, bundleId], { env });
  await sleep(1000); // the last lines through the log
  log.stop();
  await log.done;
  return { device: device.udid, pid, reason };
}

/** Whether a process of this machine exists (signal 0 checks without sending; EPERM: it exists, it is not ours) */
function processAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error.code === 'EPERM';
  }
}


/** A process's whole output (simctl's short commands) */
export function captureProcess(command, args, { env }) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', (c) => { stdout += c; });
    child.stderr.setEncoding('utf8').on('data', (c) => { stderr += c; });
    child.on('error', (error) => reject(new BuildError(`Could not start ${command}: ${error.message}`)));
    child.on('close', (code) => resolve({ code: code ?? 1, stdout, stderr }));
  });
}
