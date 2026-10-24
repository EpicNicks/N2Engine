// Node's real WebSocket client -> native C++ transport, or the compiled Emscripten client -> that same endpoint.
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import assert from 'node:assert/strict';

const [serverPath, wasmClient] = process.argv.slice(2);
assert(serverPath, 'usage: node scripts/websocket-smoke.mjs <native-echo> [wasm-client.js]');
const server = spawn(serverPath, [], { stdio: ['ignore', 'pipe', 'inherit'] });
const timeout = setTimeout(() => { server.kill(); throw new Error('WebSocket smoke timed out'); }, 25000);
const serverExit = once(server, 'exit');
try {
  let output = '';
  const port = await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.once('exit', code => reject(new Error(`native server exited before ready: ${code}`)));
    server.stdout.on('data', chunk => {
      output += chunk.toString();
      const match = output.match(/WebSocket ready port=(\d+)/);
      if (match) resolve(Number(match[1]));
    });
  });
  const url = `ws://127.0.0.1:${port}/game`;
  if (wasmClient) {
    const client = spawn(process.execPath, [wasmClient, url], { stdio: 'inherit' });
    assert.equal((await once(client, 'exit'))[0], 0, 'Emscripten client');
  } else {
    const ws = new WebSocket(url, 'n2net');
    ws.binaryType = 'arraybuffer';
    const next = () => new Promise((resolve, reject) => {
      ws.addEventListener('message', e => resolve(new Uint8Array(e.data)), { once: true });
      ws.addEventListener('error', reject, { once: true });
    });
    await once(ws, 'open');
    const auth = new Uint8Array(8 + 'smoke-only-token'.length);
    auth.set(new TextEncoder().encode('N2WS'));
    new DataView(auth.buffer).setUint32(4, 7, true);
    auth.set(new TextEncoder().encode('smoke-only-token'), 8);
    let reply = next(); ws.send(auth);
    assert.equal(new TextDecoder().decode(await reply), 'N2OK');
    for (const size of [1, 125, 126, 1200]) {
      const payload = Uint8Array.from({ length: size }, (_, i) => i & 255);
      reply = next(); ws.send(payload);
      assert.deepEqual(await reply, payload);
    }
    ws.send(new TextEncoder().encode('quit'));
    ws.close();
  }
  assert.equal((await serverExit)[0], 0, 'native server');
  console.log('WebSocket transport smoke passed');
} finally {
  clearTimeout(timeout);
  if (server.exitCode === null) server.kill();
}
