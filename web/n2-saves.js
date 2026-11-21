// Persistent saves for the web player (issue #423; docs/web-build.html#saves). Linked into index.js with --pre-js
// (web/CMakeLists.txt), so it runs inside the Emscripten module and sees FS, IDBFS and ENV. It is NOT a module and has no
// exports: scripts/web-saves-test.mjs reads this file as text and evaluates it with mock FS/IDBFS objects.
//
// What it does:
//   1. points the engine's user data folder at a fixed directory: ENV.XDG_DATA_HOME is set, and
//      ProjectFile::UserDataBase() returns $XDG_DATA_HOME/n2engine, so user:// is <dir>/n2engine/<project>-<id>/;
//   2. before the engine starts (a run dependency of the module, so the factory's promise, and with it the Start
//      button, waits for it) mounts IDBFS at <dir> and fills it from IndexedDB with FS.syncfs(true);
//   3. after the engine writes (wrapped FS.close of a stream opened for writing, FS.unlink/rmdir/rename/mkdir/writeFile
//      under <dir>) it writes back with FS.syncfs(false), debounced (a quiet period, and at most MAX_WAIT_MS after the
//      first unsaved write), one sync in flight at a time; and at once on pagehide and when the page becomes hidden.
//
// Failure never stops the game: no indexedDB (an old browser, a sandboxed frame), a populate error (Firefox private
// windows before 115, blocked site data) or a missing IDBFS leaves <dir> as plain in-memory files, with one warning.
// A later write-back that fails (quota exceeded, the database closed) logs one warning and the game goes on; the next
// write tries again. Nothing here throws into the engine.
function N2PersistentSaves(env) {
  'use strict';
  var DEBOUNCE_MS = 1000;
  var MAX_WAIT_MS = 5000;
  var dir = env.dir;
  var FS = env.FS;
  var warned = false;
  var state = { mode: 'memory', dirty: false, syncing: false, syncs: 0, failures: 0 };
  var timer = null;
  var firstDirtyAt = 0;
  var now = env.now || function () { return Date.now(); };
  var setTimer = env.setTimeout || setTimeout;
  var clearTimer = env.clearTimeout || clearTimeout;

  function warnOnce(message) {
    if (warned) return;
    warned = true;
    try { (env.warn || console.warn)('saves: ' + message); } catch (e) { /* nothing to report to */ }
  }

  function under(path) {
    return typeof path === 'string' && (path === dir || path.indexOf(dir + '/') === 0);
  }

  function writeBack() {
    if (state.mode !== 'idbfs') return;
    if (state.syncing) return; // the callback of the running sync starts another when more was written meanwhile
    if (timer !== null) { clearTimer(timer); timer = null; }
    if (!state.dirty) return;
    state.dirty = false;
    state.syncing = true;
    var done = function (error) {
      state.syncing = false;
      if (error) {
        state.failures += 1;
        state.dirty = true; // retried by the next write
        warnOnce('could not write to browser storage (' + (error.message || error) + '); saves from this page may be lost on reload');
      } else {
        state.syncs += 1;
      }
      if (state.dirty && !error) schedule();
    };
    try {
      FS.syncfs(false, done);
    } catch (error) {
      done(error);
    }
  }

  function schedule() {
    if (state.mode !== 'idbfs') return;
    var t = now();
    if (!state.dirty) { state.dirty = true; firstDirtyAt = t; }
    if (timer !== null) clearTimer(timer);
    var wait = Math.max(0, Math.min(DEBOUNCE_MS, firstDirtyAt + MAX_WAIT_MS - t));
    timer = setTimer(function () { timer = null; writeBack(); }, wait);
  }

  function markDirty() {
    if (state.mode !== 'idbfs') return;
    if (state.syncing) { state.dirty = true; return; } // the running sync's callback schedules the next
    schedule();
  }

  // Wraps FS.<name> so that a call that touches <dir> marks the saves dirty. pathOf(args) is the path it touches.
  function hook(name, pathOf) {
    var original = FS[name];
    if (typeof original !== 'function') return;
    FS[name] = function () {
      var result = original.apply(this, arguments);
      try { if (pathOf(arguments)) markDirty(); } catch (e) { /* never break a file call */ }
      return result;
    };
  }

  function installHooks() {
    var first = function (a) { return under(a[0]); };
    // A stream opened for writing: the data reaches IDBFS' in-memory node as it is written; the sync waits for close
    hook('close', function (a) {
      var stream = a[0];
      return stream && under(stream.path) && ((stream.flags & 3) !== 0);
    });
    hook('writeFile', first);
    hook('unlink', first);
    hook('rmdir', first);
    hook('mkdir', first);
    hook('rename', function (a) { return under(a[0]) || under(a[1]); });
  }

  function leaveInMemory(reason) {
    state.mode = 'memory';
    warnOnce(reason + '; saves are kept in memory and will be lost when the page closes');
  }

  // Starts the mount. `done` is called once, after the populate has finished or failed (never with an error).
  function start(done) {
    var finished = false;
    var finish = function () { if (!finished) { finished = true; try { done(); } catch (e) { /* nothing */ } } };
    try {
      if (!env.IDBFS) { leaveInMemory('IDBFS is not linked into this build'); return finish(); }
      if (!env.hasIndexedDb) { leaveInMemory('IndexedDB is not available (private window or blocked site data?)'); return finish(); }
      try { FS.mkdir(dir); } catch (e) { /* exists */ }
      FS.mount(env.IDBFS, {}, dir);
    } catch (error) {
      leaveInMemory('could not mount browser storage (' + (error && error.message || error) + ')');
      return finish();
    }
    try {
      FS.syncfs(true, function (error) {
        if (error) {
          // The mount stays: it is a working in-memory directory, only without persistence
          leaveInMemory('could not read browser storage (' + (error.message || error) + ')');
        } else {
          state.mode = 'idbfs';
          installHooks();
        }
        finish();
      });
    } catch (error) {
      leaveInMemory('could not read browser storage (' + (error && error.message || error) + ')');
      finish();
    }
  }

  function flush() { markDirty(); writeBack(); }

  // The page may go away at any moment on a phone: write what is unsaved as soon as it is hidden
  function listen(target) {
    if (!target || typeof target.addEventListener !== 'function') return;
    target.addEventListener('pagehide', function () { writeBack(); });
    target.addEventListener('visibilitychange', function () {
      if (env.document && env.document.visibilityState === 'hidden') writeBack();
    });
  }

  return { start: start, flush: flush, state: function () { return state; }, listen: listen };
}

// Glue for the real module (skipped by the test, which has no Module/FS/ENV in scope)
if (typeof Module === 'object') {
  Module['preRun'] = Module['preRun'] || [];
  Module['preRun'].push(function () {
    if (typeof FS !== 'object' || typeof ENV !== 'object') return; // not defined while this file is evaluated: checked when preRun runs
    var saves = N2PersistentSaves({
      FS: FS,
      IDBFS: typeof IDBFS === 'object' ? IDBFS : null,
      hasIndexedDb: typeof indexedDB !== 'undefined' && !!indexedDB,
      dir: '/n2-userdata',
      document: typeof document !== 'undefined' ? document : null,
      warn: function (text) { if (Module['printErr']) Module['printErr'](text); else console.warn(text); }
    });
    ENV['XDG_DATA_HOME'] = '/n2-userdata';
    addRunDependency('n2-idbfs');
    saves.start(function () { removeRunDependency('n2-idbfs'); });
    saves.listen(typeof window !== 'undefined' ? window : null);
    Module['n2Saves'] = saves;
  });
}
