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
//      first (compiles while downloading), and when that fails, e.g. because the host serves .wasm with a wrong MIME
//      type, a second download into an ArrayBuffer and WebAssembly.instantiate. Both paths need nothing from the host.
//   4. every URL is index.<ext>?v=<first 12 hex digits of the file's sha256> (cache busting: a new build is never
//      served from a stale cache; the host needs no cache headers for that). manifest.json itself is fetched with
//      cache: 'no-cache'.
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
  function create(options) {
    options = options || {};
    var fetchFn = options.fetch || (typeof fetch === 'function' ? fetch.bind(typeof self !== 'undefined' ? self : undefined) : null);
    var onProgress = options.onProgress || function () {};
    var onEvent = options.onEvent || function () {};
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
      });
    }

    function loadData() {
      return get(DATA).then(function (response) {
        if (!sizes[DATA]) sizes[DATA] = Number(response.headers.get('content-length')) || 0;
        received[DATA] = 0;
        return readAll(response, function (n) { received[DATA] += n; report(DATA); });
      }).then(function (bytes) {
        checkLength(DATA, bytes.length);
        onEvent('data-done', { bytes: bytes.length });
        // Emscripten wants an ArrayBuffer of exactly the pack
        dataBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      });
    }

    // Streaming first: the response body is counted on its way into WebAssembly.instantiateStreaming. A wrong MIME type
    // (anything but application/wasm) makes that reject before it reads a byte; so does a browser without it. Then a
    // second request reads the file into an ArrayBuffer for WebAssembly.instantiate.
    function instantiateWasm(imports) {
      received[WASM] = 0;
      return get(WASM).then(function (response) {
        if (!sizes[WASM]) sizes[WASM] = Number(response.headers.get('content-length')) || 0;
        var canStream = typeof WebAssembly.instantiateStreaming === 'function' && response.body &&
          typeof TransformStream === 'function' && typeof Response === 'function';
        if (!canStream) return { response: null, reason: 'this browser cannot stream-compile' };
        var counted = response.body.pipeThrough(new TransformStream({
          transform: function (chunk, controller) {
            received[WASM] += chunk.length;
            report(WASM);
            controller.enqueue(chunk);
          }
        }));
        var wrapped = new Response(counted, { status: 200, headers: { 'content-type': response.headers.get('content-type') || '' } });
        onEvent('wasm-streaming', {});
        return WebAssembly.instantiateStreaming(wrapped, imports).then(function (result) {
          return { result: result };
        }, function (error) {
          return { response: null, reason: String(error && error.message ? error.message : error) };
        });
      }).then(function (outcome) {
        if (outcome.result) return outcome.result;
        onEvent('wasm-fallback', { reason: outcome.reason });
        received[WASM] = 0;
        return get(WASM).then(function (response) {
          return readAll(response, function (n) { received[WASM] += n; report(WASM); });
        }).then(function (bytes) {
          checkLength(WASM, bytes.length);
          return WebAssembly.instantiate(bytes, imports);
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

  return { create: create, LoadError: LoadError, files: { manifest: MANIFEST, wasm: WASM, data: DATA } };
});
