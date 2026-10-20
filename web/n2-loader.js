// The web player's loader (stage 3 of issue #155; docs/web-build.html). A plain script, copied next to index.html in the
// package folder: the page (shell.html) uses it to download index.wasm and index.data with progress, and node's smoke
// tests (scripts/web-package-smoke.mjs, scripts/web-loader-test.mjs) load the same file with a mock fetch.
//
// What it does, in order:
//   1. fetches manifest.json (file sizes and sha256, written by scripts/web-package-finalize.mjs). A missing or broken
//      manifest is not fatal: the files are then fetched without a version and without known sizes.
//   2. downloads index.data (the asset pack) with progress and hands it to Emscripten (Module.getPreloadedPackage), so a
//      failure is a clear message here instead of an Emscripten abort.
//   3. downloads index.wasm with progress and instantiates it (Module.instantiateWasm): WebAssembly.instantiateStreaming
//      (compiles while downloading) when the host says application/wasm; otherwise (a wrong MIME type, or a browser
//      without streaming) the same response is read into an ArrayBuffer for WebAssembly.instantiate: one download
//      either way. A LinkError or CompileError is not a hosting problem (index.wasm and index.js are from different
//      builds, or the file is damaged) and gets its own message, not the fallback.
//   4. index.wasm and index.data are fetched as index.<ext>?v=<first 12 hex digits of the file's sha256>; index.js and
//      n2-loader.js are loaded by the page with the same kind of ?v= (scripts/web-package-finalize.mjs writes it into
//      index.html). manifest.json is fetched with cache: 'no-cache'. So a fresh index.html never gets an older cached
//      file of another build. index.html itself is only as fresh as the host serves it: with options.indexJsVersion
//      (the ?v= the page loaded index.js with) a page older than the manifest is reported, with a request to reload.
//      Hosts should serve index.html and manifest.json with Cache-Control: no-cache.
// Every URL is relative to the page. Nothing here needs cross-origin isolation or any header.
(function (root, factory) {
  'use strict';
  var api = factory();
  if (typeof module === 'object' && module && module.exports) module.exports = api;
  else root.N2Loader = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var MANIFEST = 'manifest.json';
  var WASM = 'index.wasm';
  var DATA = 'index.data';

  function LoadError(message, file) {
    var error = new Error(message);
    error.name = 'LoadError';
    error.file = file || null;
    return error;
  }

  // Reads a Response's body to a Uint8Array, calling onBytes(n) for every chunk (the body streams when the browser
  // allows it; otherwise one arrayBuffer() read)
  function readAll(response, onBytes) {
    if (!response.body || typeof response.body.getReader !== 'function') {
      return response.arrayBuffer().then(function (buffer) {
        onBytes(buffer.byteLength);
        return new Uint8Array(buffer);
      });
    }
    var reader = response.body.getReader();
    var chunks = [];
    var length = 0;
    function next() {
      return reader.read().then(function (step) {
        if (step.done) {
          var out = new Uint8Array(length);
          var at = 0;
          for (var i = 0; i < chunks.length; ++i) {
            out.set(chunks[i], at);
            at += chunks[i].length;
          }
          return out;
        }
        chunks.push(step.value);
        length += step.value.length;
        onBytes(step.value.length);
        return next();
      });
    }
    return next();
  }

  // options:
  //   fetch       the fetch function (default: the global one); tests pass a mock
  //   onProgress  function ({ loaded, total, file }) called as bytes arrive; total is 0 when no size is known
  //   onEvent     function (name, detail) for what the loader decided: manifest-ok, manifest-missing, wasm-streaming,
  //               wasm-fallback, wasm-done, data-done (tests assert on these; the page may log them)
  //   indexJsVersion  the ?v= the page loaded index.js with (null: not checked); a manifest whose index.js hash starts
  //               differently means the page is an older cached copy than the build on the server
  function create(options) {
    options = options || {};
    var fetchFn = options.fetch || (typeof fetch === 'function' ? fetch.bind(typeof self !== 'undefined' ? self : undefined) : null);
    var onProgress = options.onProgress || function () {};
    var onEvent = options.onEvent || function () {};
    var indexJsVersion = options.indexJsVersion || null;
    if (!fetchFn) throw LoadError('this browser has no fetch()');

    var manifest = null;
    var sizes = {};      // file name -> bytes expected (0 when unknown)
    var received = {};   // file name -> bytes received so far
    var dataBuffer = null;
    var fail = null;     // rejects run() when the wasm hook fails (the hook cannot throw into emscripten)
    var failPromise = new Promise(function (resolve, reject) { fail = reject; });
    failPromise.catch(function () {});

    function versioned(name) {
      var entry = manifest && manifest.files && manifest.files[name];
      return entry && entry.sha256 ? name + '?v=' + entry.sha256.slice(0, 12) : name;
    }

    function report(name) {
      received[name] = received[name] || 0;
      var loaded = 0;
      var total = 0;
      [WASM, DATA].forEach(function (file) {
        loaded += received[file] || 0;
        total += sizes[file] || 0;
      });
      onProgress({ loaded: loaded, total: total, file: name });
    }

    function get(name, init) {
      return fetchFn(versioned(name), init).catch(function (cause) {
        throw LoadError('Could not download ' + name + ': the network request failed (' +
          (cause && cause.message ? cause.message : cause) + '). Check the connection, and that ' + name +
          ' is next to index.html.', name);
      }).then(function (response) {
        if (!response.ok) {
          throw LoadError('Could not download ' + name + ': the server answered ' + response.status +
            (response.statusText ? ' ' + response.statusText : '') + '. Is ' + name + ' next to index.html?', name);
        }
        return response;
      });
    }

    // The size a response's body will have once read: its Content-Length, but only when the host did not compress it
    // (with a Content-Encoding the header is the encoded size and the body arrives decoded, so the size is unknown)
    function plainLength(response) {
      var encoding = String(response.headers.get('content-encoding') || '').trim().toLowerCase();
      if (encoding && encoding !== 'identity') return 0;
      return Number(response.headers.get('content-length')) || 0;
    }

    function checkLength(name, length) {
      var expected = sizes[name] || 0;
      if (expected && length !== expected) {
        throw LoadError(name + ' arrived with ' + length + ' bytes but the build says ' + expected +
          ' (a stale cached copy, or something between the server and the page changed the file).', name);
      }
    }

    function loadManifest() {
      return fetchFn(MANIFEST, { cache: 'no-cache' }).then(function (response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      }).then(function (json) {
        if (!json || typeof json.files !== 'object') throw new Error('not a manifest');
        manifest = json;
        [WASM, DATA].forEach(function (file) {
          var entry = json.files[file];
          sizes[file] = entry && entry.raw ? entry.raw : 0;
        });
        onEvent('manifest-ok', { files: Object.keys(json.files).length });
      }).catch(function (cause) {
        manifest = null;
        onEvent('manifest-missing', { reason: String(cause && cause.message ? cause.message : cause) });
      }).then(function () {
        var entry = manifest && manifest.files['index.js'];
        var built = entry && entry.sha256 ? entry.sha256.slice(0, 12) : null;
        if (indexJsVersion && built && built !== indexJsVersion) {
          throw LoadError('This page is an older copy than the build on the server (it loaded index.js ?v=' + indexJsVersion +
            ', the build has ' + built + '). Reload the page. (Hosts: serve index.html and manifest.json with ' +
            'Cache-Control: no-cache.)', 'index.html');
        }
      });
    }

    function loadData() {
      return get(DATA).then(function (response) {
        if (!sizes[DATA]) sizes[DATA] = plainLength(response);
        received[DATA] = 0;
        return readAll(response, function (n) { received[DATA] += n; report(DATA); });
      }).then(function (bytes) {
        checkLength(DATA, bytes.length);
        onEvent('data-done', { bytes: bytes.length });
        // Emscripten wants an ArrayBuffer of exactly the pack
        dataBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      });
    }

    function contentType(response) {
      return String(response.headers.get('content-type') || '').split(';')[0].trim().toLowerCase();
    }

    // A LinkError or CompileError from instantiating means index.wasm does not fit this index.js (two different builds,
    // usually an older cached page or index.js) or is damaged: no download fixes that, so no fallback for it
    function isModuleError(error) {
      return !!error && (error instanceof WebAssembly.LinkError || error instanceof WebAssembly.CompileError);
    }
    function moduleError(error) {
      if (error && error.name === 'LoadError') return error;
      return LoadError('index.wasm could not be used with this index.js (' + String(error && error.message ? error.message : error) +
        '): the two are from different builds (an older cached copy of the page or of index.js) or index.wasm is damaged. ' +
        'Reload the page. (Hosts: serve index.html and manifest.json with Cache-Control: no-cache.)', WASM);
    }

    // Streaming when the host says application/wasm: the body is counted on its way into
    // WebAssembly.instantiateStreaming. With any other type (instantiateStreaming would reject it) or a browser without
    // streaming, the same response is read into an ArrayBuffer for WebAssembly.instantiate: no second request.
    function instantiateWasm(imports) {
      received[WASM] = 0;
      function count(n) { received[WASM] += n; report(WASM); }
      function fromBytes(bytes) {
        checkLength(WASM, bytes.length);
        // Only a real module error is "different builds"; anything else (an out-of-memory RangeError, say) keeps its own message
        return WebAssembly.instantiate(bytes, imports).catch(function (error) { throw isModuleError(error) ? moduleError(error) : error; });
      }
      return get(WASM).then(function (response) {
        if (!sizes[WASM]) sizes[WASM] = plainLength(response);
        var type = contentType(response);
        var canStream = typeof WebAssembly.instantiateStreaming === 'function' && response.body &&
          typeof TransformStream === 'function' && typeof Response === 'function';
        if (!canStream || type !== 'application/wasm') {
          onEvent('wasm-fallback', { reason: canStream ? 'served as ' + (type || 'no type') + ', not application/wasm'
                                                       : 'this browser cannot stream-compile' });
          return readAll(response, count).then(fromBytes);
        }
        var abandoned = false;
        var counter = null;
        var counted = response.body.pipeThrough(new TransformStream({
          start: function (controller) { counter = controller; },
          transform: function (chunk, controller) {
            if (abandoned) return;
            count(chunk.length);
            controller.enqueue(chunk);
          }
        }));
        var wrapped = new Response(counted, { status: 200, headers: { 'content-type': type } });
        onEvent('wasm-streaming', {});
        return WebAssembly.instantiateStreaming(wrapped, imports).catch(function (error) {
          if (isModuleError(error)) throw moduleError(error);
          // Rejected for another reason, with the right type: read the file again into an ArrayBuffer. Whatever is left
          // of the first body is cancelled first, so the two downloads do not run side by side. The wrapping stream is
          // locked by now (instantiateStreaming read it), so it is errored from inside: that releases its reader and the
          // pipe from the network body cancels that body, once.
          abandoned = true;
          try { counter.error(error); } catch (ignored) { /* already closed or errored */ }
          onEvent('wasm-fallback', { reason: String(error && error.message ? error.message : error) });
          received[WASM] = 0;
          return get(WASM).then(function (second) { return readAll(second, count); }).then(fromBytes);
        });
      }).then(function (result) {
        checkLength(WASM, received[WASM]);
        onEvent('wasm-done', { bytes: received[WASM] });
        return result;
      });
    }

    // Emscripten's module settings that make it use this loader
    var hooks = {
      locateFile: function (name) { return versioned(name); },
      instantiateWasm: function (imports, receive) {
        instantiateWasm(imports).then(function (result) {
          receive(result.instance, result.module);
        }, function (error) {
          fail(error instanceof Error ? error : LoadError(String(error)));
        });
        return {};
      },
      getPreloadedPackage: function (name) {
        // Emscripten asks for the pack by the name locateFile gave it (index.data?v=...)
        return String(name).split('?')[0].split('/').pop() === DATA ? dataBuffer : null;
      }
    };

    // run(factory, settings): downloads what the engine needs, then calls the Emscripten factory (createN2Player) with
    // settings plus the hooks above. Resolves with the engine instance; rejects with a LoadError (or the engine's own
    // error) with a message fit to show to the person.
    function run(factoryFn, settings) {
      var merged = {};
      Object.keys(settings || {}).forEach(function (key) { merged[key] = settings[key]; });
      Object.keys(hooks).forEach(function (key) { merged[key] = hooks[key]; });
      return loadManifest().then(function () {
        return loadData();
      }).then(function () {
        return Promise.race([factoryFn(merged), failPromise]);
      });
    }

    return { run: run, hooks: hooks, manifest: function () { return manifest; } };
  }

  // ---- Audio unlock (stage 4) ----
  //
  // Browsers keep an AudioContext "suspended" until the page has had a user gesture, and suspend it again for a hidden tab.
  // Emscripten's OpenAL (the web build's audio) creates its AudioContext inside the engine, where the page cannot reach it,
  // so install() wraps the page's AudioContext constructor (window.AudioContext and webkitAudioContext) to remember every
  // context made after that. Call it before the engine starts (the page does, before loader.run).
  //
  //   * Every remembered context arms gesture listeners on the document (whatever its state). Only events that grant
  //     user activation in HTML count: keydown (not Escape), a mouse pointerdown, a pointerup that is not from a mouse
  //     (touch, pen) and touchend. A touch or pen pointerdown, a mouse pointerup and Escape are ignored: they neither
  //     resume nor disarm. The first counting event calls resume() on each context that is not running, closed or
  //     already resuming from an earlier gesture. The listeners are removed once no context needs a resume (none
  //     suspended, no gesture's resume pending), so a later gesture does nothing; a gesture that finds every context
  //     running removes them at once. A gesture's resume() that is refused, throws, or settles with the context still
  //     not running keeps them armed for the next counting gesture, at most MAX_RESUME_TRIES such refusals in a row (a
  //     browser that never allows it is not retried forever); any successful resume starts the count again. A context
  //     made later arms them again.
  //   * A hidden tab suspends the running contexts; the tab becoming visible resumes the ones this suspended and arms
  //     the listeners while one is not running yet. That resume is not a gesture: its refusal does not count, and while
  //     it is pending a gesture still calls resume() (calling it twice is harmless).
  //   * Nothing throws: no AudioContext (no WebAudio), a constructor that throws (the engine sees that, as it would
  //     without this wrapper), a resume() that rejects or throws, a context that is already closed.
  //
  // options: window (the global scope: default self), document (default window.document), onEvent(name, detail) with
  // audio-installed, audio-unavailable, audio-context-created, audio-resumed, audio-resume-failed, audio-suspended-hidden,
  // audio-resumed-visible. Returns { install, uninstall, contexts, isArmed }.
  var GESTURES = ['pointerdown', 'pointerup', 'keydown', 'touchend'];
  var MAX_RESUME_TRIES = 3;

  function createAudioUnlock(options) {
    options = options || {};
    var win = options.window || (typeof self !== 'undefined' ? self : null);
    var doc = options.document || (win && win.document) || null;
    var onEvent = options.onEvent || function () {};
    var contexts = [];
    var suspendedHere = [];
    var pending = []; // contexts whose resume() promise has not settled
    var failures = 0;
    var armed = false;
    var installed = false;
    var originals = null;
    var listenerOptions = { capture: true, passive: true };

    function emit(name, detail) {
      try { onEvent(name, detail || {}); } catch (ignored) { /* a broken listener is not the unlock's problem */ }
    }
    function reasonOf(error) { return String(error && error.message ? error.message : error); }
    function isRunning(context) { return context.state === 'running'; }
    function isClosed(context) { return context.state === 'closed'; }

    function arm() {
      if (armed || !doc) return;
      armed = true;
      GESTURES.forEach(function (name) { doc.addEventListener(name, onGesture, listenerOptions); });
    }
    function disarm() {
      if (!armed || !doc) return;
      armed = false;
      GESTURES.forEach(function (name) { doc.removeEventListener(name, onGesture, listenerOptions); });
    }

    function needsResume(context) { return !isRunning(context) && !isClosed(context); }

    // Arms while a context still needs a resume or a gesture's resume is pending, disarms otherwise or once the gesture
    // refusals in a row are used up.
    function update() {
      if (failures > MAX_RESUME_TRIES) { disarm(); return; }
      if (pending.length > 0 || contexts.some(needsResume)) arm(); else disarm();
    }

    // fromGesture: started by an activating event. Only those count as pending (so a resume the tab started when it
    // became visible, which may stay pending until an allowed one, never stops a gesture from asking again) and only
    // their refusals use up MAX_RESUME_TRIES. A successful resume, from either, starts the count again.
    function resume(context, eventName, fromGesture) {
      var result;
      try {
        result = context.resume();
      } catch (error) {
        refused(error, fromGesture);
        return;
      }
      if (result && typeof result.then === 'function') {
        if (fromGesture) pending.push(context);
        result.then(function () {
          if (fromGesture) settled(context);
          if (needsResume(context)) refused(new Error('still ' + context.state + ' after resume()'), fromGesture);
          else succeeded(eventName);
        }, function (error) {
          if (fromGesture) settled(context);
          refused(error, fromGesture);
        });
      } else if (needsResume(context)) {
        refused(new Error('still ' + context.state + ' after resume()'), fromGesture);
      } else {
        succeeded(eventName);
      }
    }
    function settled(context) {
      var index = pending.indexOf(context);
      if (index >= 0) pending.splice(index, 1);
    }
    function succeeded(eventName) {
      failures = 0;
      emit(eventName, {});
      update();
    }
    function refused(error, fromGesture) {
      emit('audio-resume-failed', { reason: reasonOf(error) });
      if (fromGesture) ++failures;
      update();
    }

    // The events that grant user activation (HTML "activation triggering input event"); the rest are ignored.
    function isActivation(event) {
      if (!event) return false;
      switch (event.type) {
        case 'pointerdown': return event.pointerType === 'mouse';
        case 'pointerup': return event.pointerType !== 'mouse';
        case 'keydown': return event.key !== 'Escape';
        case 'touchend': return true;
        default: return false;
      }
    }

    function onGesture(event) {
      if (!isActivation(event)) return;
      contexts.filter(function (context) { return needsResume(context) && pending.indexOf(context) < 0; })
              .forEach(function (context) { resume(context, 'audio-resumed', true); });
      update();
    }

    function onVisibility() {
      if (!doc) return;
      if (doc.visibilityState === 'hidden' || doc.hidden === true) {
        contexts.forEach(function (context) {
          if (!isRunning(context) || suspendedHere.indexOf(context) >= 0) return;
          suspendedHere.push(context);
          try {
            var result = context.suspend();
            if (result && typeof result.catch === 'function') result.catch(function () {});
          } catch (ignored) { /* already closed */ }
          emit('audio-suspended-hidden', {});
        });
      } else {
        var back = suspendedHere;
        suspendedHere = [];
        back.forEach(function (context) {
          if (isClosed(context)) return;
          resume(context, 'audio-resumed-visible', false);
        });
        // A context still not running (refused, pending, or suspended by the browser) waits for the next gesture
        update();
      }
    }

    function track(context) {
      contexts.push(context);
      emit('audio-context-created', { state: context.state });
      arm();
    }

    function wrap(Native) {
      // A function (not a class) so that `new AudioContext(options)` and a subclass-free call both work; the prototype is
      // the native one, so instanceof and every method keep working. Reflect.construct builds the native object.
      function Tracked() {
        var context = Reflect.construct(Native, arguments, new.target || Tracked);
        try { track(context); } catch (ignored) { /* tracking must never break the engine's own context */ }
        return context;
      }
      Tracked.prototype = Native.prototype;
      Object.setPrototypeOf(Tracked, Native);
      return Tracked;
    }

    function install() {
      if (installed) return true;
      if (!win || !(win.AudioContext || win.webkitAudioContext) || typeof Reflect !== 'object') {
        emit('audio-unavailable', { reason: 'no AudioContext here' });
        return false;
      }
      installed = true;
      originals = { AudioContext: win.AudioContext, webkitAudioContext: win.webkitAudioContext };
      var wrapped = [];
      ['AudioContext', 'webkitAudioContext'].forEach(function (name) {
        var Native = originals[name];
        if (!Native) return;
        for (var i = 0; i < wrapped.length; ++i) {
          if (wrapped[i].Native === Native) { win[name] = wrapped[i].Tracked; return; }
        }
        var Tracked = wrap(Native);
        wrapped.push({ Native: Native, Tracked: Tracked });
        win[name] = Tracked;
      });
      if (doc) doc.addEventListener('visibilitychange', onVisibility, listenerOptions);
      emit('audio-installed', {});
      return true;
    }

    function uninstall() {
      if (!installed) return;
      installed = false;
      disarm();
      if (doc) doc.removeEventListener('visibilitychange', onVisibility, listenerOptions);
      ['AudioContext', 'webkitAudioContext'].forEach(function (name) {
        if (originals[name]) win[name] = originals[name];
      });
      originals = null;
    }

    return {
      install: install,
      uninstall: uninstall,
      contexts: function () { return contexts.slice(); },
      isArmed: function () { return armed; }
    };
  }

  return { create: create, createAudioUnlock: createAudioUnlock, LoadError: LoadError, files: { manifest: MANIFEST, wasm: WASM, data: DATA } };
});
