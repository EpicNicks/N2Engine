// Web (Emscripten) test executables only: give the program the environment of the node process that runs it.
//
// Emscripten does not pass the host's environment variables to the program: getenv sees only a fixed set (HOME is
// /home/web_user, TMPDIR is unset). With -sNODERAWFS the files are the host's, so a test that asks for the user's home
// or the temporary folder would then point at /home/web_user, which does not exist. Copying process.env first makes
// getenv behave as in a native run. Added by tests/CMakeLists.txt with --pre-js; never part of a shipped web build.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  if (typeof process !== 'undefined' && process.env) {
    for (var name in process.env) {
      ENV[name] = process.env[name];
    }
  }
});
