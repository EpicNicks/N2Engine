// Optional real-browser proof for the compiled Emscripten transport. Uses a separate browser profile.
// node websocket-browser-smoke.mjs <echo-executable> <client-directory> <playwright-prefix>
// N2_WS_ECHO_ARGS may be a JSON array of extra arguments before the Origin (e.g. a WSL launcher).
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createConnection } from 'node:net';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import assert from 'node:assert/strict';
const [echo, clientDir, playwrightPrefix] = process.argv.slice(2);
const require = createRequire(path.resolve(playwrightPrefix, 'package.json'));
const { chromium } = require('playwright');
let wsUrl;
const http = createServer(async (req, res) => {
  try {
    const name = new URL(req.url, 'http://localhost').pathname;
    if (name === '/') {
      res.setHeader('Content-Type', 'text/html');
      res.end(`<!doctype html><script>var Module={arguments:[${JSON.stringify(wsUrl)}],
        onExit:status=>window.__done={status},onAbort:error=>window.__done={status:1,error:String(error)}};</script>
        <script src="/websocket_client.js"></script>`);
    } else if (name === '/websocket_client.js' || name === '/websocket_client.wasm') {
      res.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      res.end(await readFile(path.join(clientDir, name.slice(1))));
    } else { res.writeHead(404); res.end(); }
  } catch (error) { res.writeHead(500); res.end(String(error)); }
});
await new Promise(resolve => http.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${http.address().port}`;
const server = spawn(echo, [...JSON.parse(process.env.N2_WS_ECHO_ARGS || '[]'), origin], { stdio: ['ignore','pipe','inherit'] });
let browser;
try {
  const port = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('native listener not ready')), 15000);
    let output = '';
    server.on('error', reject);
    server.stdout.on('data', data => {
      output += data;
      const match = output.match(/WebSocket ready port=(\d+)/);
      if (match) { clearTimeout(timer); resolve(Number(match[1])); }
    });
    server.on('exit', code => { clearTimeout(timer); reject(new Error(`native listener exited ${code}`)); });
  });
  wsUrl = `ws://127.0.0.1:${port}/game`;
  // A WSL/VM launcher can report its listener before the host's localhost forwarding is ready.
  const deadline = Date.now() + 5000;
  while (true) {
    try {
      await new Promise((resolve, reject) => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.setTimeout(500);
        socket.once('connect', () => { socket.end(); resolve(); });
        socket.once('error', error => { socket.destroy(); reject(error); });
        socket.once('timeout', () => { socket.destroy(); reject(new Error('listener probe timed out')); });
      });
      break;
    } catch (error) {
      if (Date.now() >= deadline) throw error;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  }
  browser = await chromium.launch({ headless: true, executablePath: process.env.N2_BROWSER_EXECUTABLE || undefined });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.goto(origin);
  await page.waitForFunction(() => window.__done, null, { timeout: 15000 });
  const result = await page.evaluate(() => window.__done);
  assert.equal(result.status, 0, JSON.stringify(result));
  assert.deepEqual(errors, []);
  console.log('Real-browser Emscripten WebSocket smoke passed');
} finally {
  await browser?.close();
  server.kill();
  await new Promise(resolve => http.close(resolve));
}
