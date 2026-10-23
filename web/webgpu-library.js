// Bridge for the pinned emdawnwebgpu package. Keep its JS object helpers inside
// an Emscripten library so Closure sees the dependencies and retains them.
addToLibrary({
  n2_webgpu_import_device__deps: ['$WebGPU'],
  n2_webgpu_import_device: function () {
    var session = Module['n2GpuSession'];
    var device = session && session.device;
    return device ? WebGPU.importJsDevice(device) : 0;
  },
  n2_webgpu_present__deps: ['$WebGPU'],
  n2_webgpu_present: function (texture) {
    var session = Module['n2GpuSession'];
    if (session) session.present(WebGPU.getJsObject(texture));
  }
});
