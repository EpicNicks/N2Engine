// Tests the audio unlock of web/n2-loader.js (createAudioUnlock) under node with a mock AudioContext and a mock document.
// Usage: node scripts/web-audio-unlock-test.mjs. Nothing here needs a build or a browser.
// Covered: no AudioContext (install reports it, nothing throws), the constructor wrapper (instanceof, arguments, both
// names, uninstall puts the originals back, install twice), resume() called once per suspended context on the first
// pointerdown, keydown or touchend and the listeners removed (a second gesture does nothing), a context that is already
// running is not resumed, a context made after the unlock re-arms it, a refused or throwing resume() is tolerated and
// retried on the next gesture a bounded number of times, a constructor that throws is the engine's to handle, the hidden
// tab suspending and the visible tab resuming only what it suspended.
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const N2Loader = createRequire(import.meta.url)(path.join(here, '..', 'web', 'n2-loader.js'));

let failures = 0;
function check(condition, message) {
  if (!condition) {
    failures += 1;
    console.error(`  FAIL: ${message}`);
  }
}

// A document that records its listeners and can fire them
function mockDocument() {
  const listeners = new Map(); // type -> Set of functions
  const doc = {
    visibilityState: 'visible',
    hidden: false,
    adds: 0,
    removes: 0,
    addEventListener(type, fn) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(fn);
      doc.adds += 1;
    },
    removeEventListener(type, fn) {
      if (listeners.has(type) && listeners.get(type).delete(fn)) doc.removes += 1;
    },
    count(type) { return listeners.has(type) ? listeners.get(type).size : 0; },
    fire(type) { for (const fn of [...(listeners.get(type) || [])]) fn({ type }); },
    setVisible(visible) {
      doc.visibilityState = visible ? 'visible' : 'hidden';
      doc.hidden = !visible;
      doc.fire('visibilitychange');
    },
  };
  return doc;
}

// A mock AudioContext class. `behaviour` decides what resume() does; contexts made are in `made`.
function mockAudio(behaviour = {}) {
  const made = [];
  class MockAudioContext {
    constructor(options) {
      if (behaviour.constructorThrows) throw new Error('NotSupportedError');
      this.options = options;
      this.state = behaviour.startState || 'suspended';
      this.resumeCalls = 0;
      this.suspendCalls = 0;
      made.push(this);
    }
    resume() {
      this.resumeCalls += 1;
      if (behaviour.resumeThrows) throw new Error('InvalidStateError');
      if (behaviour.resumeRejects) return Promise.reject(new Error('NotAllowedError'));
      this.state = 'running';
      return Promise.resolve();
    }
    suspend() {
      this.suspendCalls += 1;
      this.state = 'suspended';
      return Promise.resolve();
    }
  }
  return { MockAudioContext, made };
}

function setup(behaviour, { webkit = false } = {}) {
  const { MockAudioContext, made } = mockAudio(behaviour);
  const doc = mockDocument();
  const win = { document: doc };
  if (webkit) win.webkitAudioContext = MockAudioContext; else win.AudioContext = MockAudioContext;
  const events = [];
  const unlock = N2Loader.createAudioUnlock({ window: win, document: doc, onEvent: (name, detail) => events.push({ name, detail }) });
  return { win, doc, made, events, unlock, MockAudioContext };
}

const tick = () => new Promise((resolve) => setImmediate(resolve));
const names = (events) => events.map((e) => e.name);

async function scenario(name, fn) {
  const before = failures;
  try {
    await fn();
  } catch (error) {
    failures += 1;
    console.error(`  FAIL: threw ${error && error.stack ? error.stack : error}`);
  }
  console.log(`${failures === before ? 'ok  ' : 'FAIL'} ${name}`);
}

await scenario('no AudioContext: install says so and nothing throws', async () => {
  const doc = mockDocument();
  const events = [];
  const unlock = N2Loader.createAudioUnlock({ window: { document: doc }, document: doc, onEvent: (n, d) => events.push({ name: n, detail: d }) });
  check(unlock.install() === false, 'install returns false');
  check(names(events).join() === 'audio-unavailable', `events were ${names(events)}`);
  check(doc.adds === 0, 'no listener added');
  unlock.uninstall();
});

await scenario('the wrapper builds real contexts, passes arguments, keeps instanceof, and uninstall restores', async () => {
  const { win, made, MockAudioContext, unlock } = setup();
  check(unlock.install() === true, 'install returns true');
  check(win.AudioContext !== MockAudioContext, 'the constructor is wrapped');
  const context = new win.AudioContext({ latencyHint: 'interactive' });
  check(context instanceof MockAudioContext, 'a mock instance');
  check(context instanceof win.AudioContext, 'instanceof the wrapper');
  check(made.length === 1 && made[0].options && made[0].options.latencyHint === 'interactive', 'arguments reach the constructor');
  check(unlock.contexts().length === 1, 'the context is remembered');
  check(unlock.install() === true && win.AudioContext !== MockAudioContext, 'install twice is harmless');
  new win.AudioContext();
  check(unlock.contexts().length === 2, 'a second install did not wrap twice (one remembered context each)');
  unlock.uninstall();
  check(win.AudioContext === MockAudioContext, 'uninstall restores the constructor');
  unlock.uninstall();
});

await scenario('webkitAudioContext alone is wrapped too', async () => {
  const { win, unlock, made } = setup({}, { webkit: true });
  check(unlock.install() === true, 'install returns true');
  new win.webkitAudioContext();
  check(made.length === 1 && unlock.contexts().length === 1, 'remembered');
  unlock.uninstall();
});

await scenario('resume is called once on the first gesture, then the listeners are gone', async () => {
  for (const gesture of ['pointerdown', 'keydown', 'touchend']) {
    const { win, doc, made, events, unlock } = setup();
    unlock.install();
    check(doc.count(gesture) === 0, `${gesture}: no gesture listener before a context exists`);
    new win.AudioContext();
    check(unlock.isArmed(), `${gesture}: armed once a context exists`);
    check(doc.count('pointerdown') === 1 && doc.count('keydown') === 1 && doc.count('touchend') === 1, `${gesture}: three gesture listeners`);
    doc.fire(gesture);
    await tick();
    check(made[0].resumeCalls === 1, `${gesture}: resume called once (${made[0].resumeCalls})`);
    check(made[0].state === 'running', `${gesture}: running`);
    check(!unlock.isArmed(), `${gesture}: disarmed`);
    check(doc.count('pointerdown') === 0 && doc.count('keydown') === 0 && doc.count('touchend') === 0, `${gesture}: listeners removed`);
    doc.fire('pointerdown');
    doc.fire('keydown');
    await tick();
    check(made[0].resumeCalls === 1, `${gesture}: a later gesture does nothing`);
    check(names(events).includes('audio-resumed'), `${gesture}: audio-resumed reported`);
    unlock.uninstall();
  }
});

await scenario('a running context is not resumed, and the listeners still go', async () => {
  const { win, doc, made, unlock } = setup({ startState: 'running' });
  unlock.install();
  new win.AudioContext();
  doc.fire('keydown');
  await tick();
  check(made[0].resumeCalls === 0, 'resume not called');
  check(!unlock.isArmed() && doc.count('keydown') === 0, 'disarmed');
  unlock.uninstall();
});

await scenario('several suspended contexts: each resumed once', async () => {
  const { win, doc, made, unlock } = setup();
  unlock.install();
  new win.AudioContext();
  new win.AudioContext();
  check(doc.count('keydown') === 1, 'one listener set, however many contexts');
  doc.fire('keydown');
  await tick();
  check(made.every((c) => c.resumeCalls === 1), 'each resumed once');
  unlock.uninstall();
});

await scenario('a context made after the unlock arms it again', async () => {
  const { win, doc, made, unlock } = setup();
  unlock.install();
  new win.AudioContext();
  doc.fire('pointerdown');
  await tick();
  check(!unlock.isArmed(), 'disarmed');
  new win.AudioContext();
  check(unlock.isArmed(), 're-armed by the new context');
  doc.fire('touchend');
  await tick();
  check(made[0].resumeCalls === 1 && made[1].resumeCalls === 1, 'the first is not resumed again, the second once');
  unlock.uninstall();
});

await scenario('uninstall removes the gesture and visibility listeners', async () => {
  const { win, doc, unlock } = setup();
  unlock.install();
  new win.AudioContext();
  check(doc.count('visibilitychange') === 1, 'visibility listener installed');
  unlock.uninstall();
  check(doc.count('pointerdown') === 0 && doc.count('keydown') === 0 && doc.count('touchend') === 0 && doc.count('visibilitychange') === 0, 'all removed');
  check(doc.adds === doc.removes, `adds ${doc.adds} equal removes ${doc.removes}`);
});

await scenario('a rejected resume is tolerated and retried on the next gesture, a bounded number of times', async () => {
  const { win, doc, made, events, unlock } = setup({ resumeRejects: true });
  unlock.install();
  new win.AudioContext();
  let gestures = 0;
  for (let i = 0; i < 10; ++i) {
    if (!unlock.isArmed()) break;
    doc.fire('keydown');
    gestures += 1;
    await tick();
  }
  check(made[0].resumeCalls === gestures, 'one resume per gesture');
  check(gestures === 4, `gave up after 4 gestures (3 refusals re-arm, the 4th does not): ${gestures}`);
  check(!unlock.isArmed(), 'disarmed at the end');
  check(names(events).filter((n) => n === 'audio-resume-failed').length === 4, 'every refusal reported');
  unlock.uninstall();
});

await scenario('a throwing resume is tolerated', async () => {
  const { win, doc, made, events, unlock } = setup({ resumeThrows: true });
  unlock.install();
  new win.AudioContext();
  doc.fire('pointerdown');
  await tick();
  check(made[0].resumeCalls === 1, 'resume was tried');
  check(names(events).includes('audio-resume-failed'), 'reported');
  check(unlock.isArmed(), 're-armed for the next gesture');
  unlock.uninstall();
});

await scenario('a constructor that throws reaches the caller and is not remembered', async () => {
  const { win, unlock } = setup({ constructorThrows: true });
  unlock.install();
  let error = null;
  try { new win.AudioContext(); } catch (e) { error = e; }
  check(error && error.message === 'NotSupportedError', 'the engine sees the native error');
  check(unlock.contexts().length === 0 && !unlock.isArmed(), 'nothing remembered, not armed');
  unlock.uninstall();
});

await scenario('a closed context is not resumed', async () => {
  const { win, doc, made, unlock } = setup();
  unlock.install();
  new win.AudioContext();
  made[0].state = 'closed';
  doc.fire('keydown');
  await tick();
  check(made[0].resumeCalls === 0, 'not resumed');
  unlock.uninstall();
});

await scenario('a hidden tab suspends the running contexts, visible resumes only those', async () => {
  const { win, doc, made, events, unlock } = setup({ startState: 'running' });
  unlock.install();
  new win.AudioContext();
  new win.AudioContext();
  made[1].state = 'suspended'; // the browser keeps this one suspended: the tab must not "resume" it
  doc.setVisible(false);
  await tick();
  check(made[0].suspendCalls === 1 && made[0].state === 'suspended', 'the running one is suspended');
  check(made[1].suspendCalls === 0, 'the suspended one is left alone');
  doc.setVisible(false);
  check(made[0].suspendCalls === 1, 'hidden twice suspends once');
  doc.setVisible(true);
  await tick();
  check(made[0].resumeCalls === 1 && made[0].state === 'running', 'resumed when visible');
  check(made[1].resumeCalls === 0, 'the other one is not resumed by the tab');
  check(names(events).includes('audio-suspended-hidden') && names(events).includes('audio-resumed-visible'), 'both reported');
  unlock.uninstall();
});

await scenario('a refused resume when the tab is visible again waits for a gesture', async () => {
  const { win, doc, made, unlock } = setup({ startState: 'running', resumeRejects: true });
  unlock.install();
  new win.AudioContext();
  doc.setVisible(false);
  await tick();
  doc.setVisible(true);
  await tick();
  check(made[0].resumeCalls === 1, 'tried once on visible');
  check(unlock.isArmed(), 'armed: the next gesture tries again');
  unlock.uninstall();
});

if (failures > 0) {
  console.error(`web audio unlock test: ${failures} failure(s)`);
  process.exit(1);
}
console.log('web audio unlock test: OK');
