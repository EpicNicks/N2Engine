// Running an Android package on a device (build targets stage 9; docs/android.html#run): pick the device, install the APK,
// start the game, and stream its log (logcat, for the game's process only) as n2build log events until the game exits.
//
//   findAdb        the SDK's platform-tools/adb
//   parseDevices   `adb devices -l` -> [{serial, state, model}]
//   pickDevice     --device, else the one device attached; several need --device
//   runOnDevice    install, start, follow the process, logcat
import { spawn } from 'node:child_process';
import path from 'node:path';

import { BuildError } from './report.mjs';

export const ACTIVITY = 'com.n2engine.player.N2Activity';

/** <sdk>/platform-tools/adb(.exe), or null */
export function findAdb(androidSdk, ctx) {
  if (!androidSdk) return null;
  const join = ctx.platform === 'win32' ? path.win32.join : path.posix.join;
  const exe = join(androidSdk, 'platform-tools', ctx.platform === 'win32' ? 'adb.exe' : 'adb');
  return ctx.exists(exe) ? exe : null;
}

/** `adb devices -l` output -> the devices, in the order adb lists them */
export function parseDevices(text) {
  const devices = [];
  for (const line of (text ?? '').split(/\r?\n/)) {
    if (line.startsWith('List of devices') || line.startsWith('*') || line.trim() === '') continue;
    const [serial, state, ...rest] = line.trim().split(/\s+/);
    if (!serial || !state) continue;
    const model = rest.find((part) => part.startsWith('model:'))?.slice('model:'.length) ?? null;
    devices.push({ serial, state, model });
  }
  return devices;
}

/** The device to use; throws a BuildError that says what to do */
export function pickDevice(devices, wanted) {
  const ready = devices.filter((d) => d.state === 'device');
  const describe = (d) => `${d.serial}${d.model ? ` (${d.model})` : ''}${d.state === 'device' ? '' : ` [${d.state}]`}`;
  if (wanted) {
    const device = devices.find((d) => d.serial === wanted);
    if (!device) throw new BuildError(`No device ${wanted} is attached (attached: ${devices.map(describe).join(', ') || 'none'})`);
    if (device.state !== 'device') {
      throw new BuildError(`Device ${wanted} is ${device.state}${device.state === 'unauthorized' ? ': accept the USB debugging prompt on the device' : ''}`);
    }
    return device;
  }
  if (ready.length === 1) return ready[0];
  if (ready.length === 0) {
    const unauthorized = devices.find((d) => d.state === 'unauthorized');
    if (unauthorized) throw new BuildError(`Device ${unauthorized.serial} is unauthorized: accept the USB debugging prompt on the device`);
    throw new BuildError('No Android device is attached: connect a phone with USB debugging on (or start an emulator), then run again');
  }
  throw new BuildError(`Several devices are attached: choose one with --device (${ready.map(describe).join(', ')})`);
}

/** A process's whole output, for the short adb commands; rejects when adb cannot start */
function capture(adb, args, { env }) {
  return new Promise((resolve, reject) => {
    const child = spawn(adb, args, { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', (c) => { stdout += c; });
    child.stderr.setEncoding('utf8').on('data', (c) => { stderr += c; });
    child.on('error', (error) => reject(new BuildError(`Could not start ${adb}: ${error.message}`)));
    child.on('close', (code) => resolve({ code: code ?? 1, stdout, stderr }));
  });
}

/**
 * Install, start and follow the game. Resolves when its process ends, or after `seconds` (then the game is stopped).
 * `exec` (capture's shape) and `stream` (a line-streaming runner returning {stop(), done}) are injectable for tests.
 */
export async function runOnDevice({ adb, apk, appId, device, env, reporter, seconds = null, exec = capture, stream = streamLines, sleep = delay, now = Date.now }) {
  const on = ['-s', device.serial];
  const run = async (args, what) => {
    const result = await exec(adb, [...on, ...args], { env });
    if (result.code !== 0 || /\bFailure\b|Error:/.test(result.stdout)) {
      throw new BuildError(`${what} failed: ${(result.stderr || result.stdout).trim().split(/\r?\n/).slice(-3).join(' ')}`);
    }
    return result;
  };
  reporter.step('install', `Installing ${path.basename(apk)} on ${device.serial}${device.model ? ` (${device.model})` : ''}`);
  await run(['install', '-r', apk], 'adb install');
  reporter.step('run', `Starting ${appId}`);
  await run(['logcat', '-c'], 'Clearing the device log');
  await run(['shell', 'am', 'start', '-W', '-n', `${appId}/${ACTIVITY}`], 'Starting the game');

  // The game's process id, once it is up (am start -W has returned, but the process can still be starting)
  const pidOf = async () => {
    const result = await exec(adb, [...on, 'shell', 'pidof', appId], { env });
    const pid = result.stdout.trim().split(/\s+/)[0];
    return result.code === 0 && /^\d+$/.test(pid) ? pid : null;
  };
  let pid = null;
  for (let i = 0; i < 20 && !pid; ++i) {
    pid = await pidOf();
    if (!pid) await sleep(250);
  }
  if (!pid) throw new BuildError(`${appId} did not start (no process); see adb logcat on the device`);

  reporter.step('logcat', `The game's log (process ${pid}); stop the game, or press Ctrl+C, to end`);
  const logcat = stream(adb, [...on, 'logcat', '-v', 'brief', `--pid=${pid}`], { env, onLine: (line) => reporter.log('logcat', line) });
  const started = now();
  let reason = 'exited';
  for (;;) {
    await sleep(1000);
    if (seconds !== null && now() - started >= seconds * 1000) {
      reason = 'time';
      await exec(adb, [...on, 'shell', 'am', 'force-stop', appId], { env });
      break;
    }
    if ((await pidOf()) !== pid) break;
  }
  logcat.stop();
  await logcat.done;
  return { pid, reason };
}

const delay = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });

/** A long-running process whose lines go to onLine; stop() ends it */
export function streamLines(command, args, { env, onLine }) {
  const child = spawn(command, args, { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let pending = '';
  child.stdout.setEncoding('utf8').on('data', (chunk) => {
    pending += chunk;
    const lines = pending.split(/\r?\n/);
    pending = lines.pop();
    for (const line of lines) onLine(line);
  });
  child.stderr.resume();
  const done = new Promise((resolve) => {
    child.on('close', () => {
      if (pending !== '') onLine(pending);
      resolve();
    });
    child.on('error', () => resolve());
  });
  return { stop: () => child.kill(), done };
}
