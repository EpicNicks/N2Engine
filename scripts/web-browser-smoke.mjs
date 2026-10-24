// Optional real-browser gate. Install the locked development tools:
// npm ci --prefix scripts/browser-tools
// node scripts/browser-tools/node_modules/playwright/cli.js install --with-deps --no-shell chromium
// node scripts/web-browser-smoke.mjs <web-package> <playwright-prefix>
// Set N2_BROWSER_EXECUTABLE to use installed Chrome/Edge, or install Playwright Chromium.
// --swiftshader --headed is the G6 CI gate under Xvfb (requires N2_READBACK_DIR).
// --unsafe-webgpu only enables blocked adapters. Default remains headless browser defaults.
// Default runs browser defaults. Evidence is retained on failures, including browser launch failures.
import { createRequire } from 'node:module';
import { browserOptions, browserLaunchArgs, requireBrowserInputs, requireSwiftShader, requireVisibleCanvas } from './web-browser-support.mjs';
const options = browserOptions(process.argv.slice(2));
const require = createRequire(import.meta.url);
const playwrightRequire = createRequire(require('node:path').join(options.tools, 'package.json'));
const { chromium } = playwrightRequire('playwright');
const { PNG } = playwrightRequire('pngjs');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = options.root;
const readbackRoot = process.env.N2_READBACK_DIR ? path.resolve(process.env.N2_READBACK_DIR) : null;
const evidenceDir = path.resolve(process.env.N2_BROWSER_EVIDENCE_DIR || path.join(root, '..', 'browser-evidence'));
fs.mkdirSync(evidenceDir, { recursive: true });
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/readback-tests.html' && readbackRoot) {
    res.setHeader('Content-Type', 'text/html');
    res.end(`<!doctype html><script src="/readback-tests/n2-webgpu-readback.js"></script><script>
      window.__readbackErrors = [];
      (async function () {
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) throw new Error('no readback test adapter');
        const device = await adapter.requestDevice();
        device.addEventListener('uncapturederror', event => window.__readbackErrors.push(String(event.error.message)));
        const engine = await createN2ReadbackTests({ noInitialRun: true, n2GpuSession: { device },
          n2ReadbackResult: (status, message) => { window.__readbackDone = { status, message }; },
          onAbort: message => { window.__readbackDone = { status: 1, message: String(message) }; } });
        engine.callMain([]);
      })().catch(error => { window.__readbackDone = { status: 1, message: String(error) }; });
    </script>`);
    return;
  }
  if (url.pathname === '/embed.html') {
    res.setHeader('Content-Type', 'text/html');
    res.end('<!doctype html><iframe id="player" style="border:0;width:100%;height:100vh" src="http://localhost:' +
      server.address().port + '/?renderer=webgpu"></iframe>');
    return;
  }
  const readbackRequest = readbackRoot && url.pathname.startsWith('/readback-tests/');
  const contentRoot = readbackRequest ? readbackRoot : root;
  const name = readbackRequest ? url.pathname.slice('/readback-tests/'.length) : url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
  const file = path.resolve(contentRoot, name);
  if (!file.startsWith(contentRoot + path.sep)) { res.writeHead(403).end(); return; }
  fs.readFile(file, (error, data) => {
    if (error) { res.writeHead(404).end(); return; }
    res.setHeader('Content-Type', file.endsWith('.wasm') ? 'application/wasm' : file.endsWith('.js') ? 'text/javascript' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream');
    res.end(data);
  });
});
const messages = [];
const report = { status: 'running', swiftshader: options.swiftshader, headless: !options.headed, stages: [], screenshots: [], pageErrors: [] };
let browser, page;
function stage(name) { report.stages.push(name); messages.push('STAGE: ' + name); console.log('Browser stage: ' + name); }
function writeEvidence() {
  fs.writeFileSync(path.join(evidenceDir, 'browser.log'), messages.join('\n'));
  fs.writeFileSync(path.join(evidenceDir, 'report.json'), JSON.stringify(report, null, 2) + '\n');
}
async function screenshot(name, visible = false) {
  const file = path.join(evidenceDir, name);
  if (visible) {
    const png = await page.locator('#canvas').screenshot({ path: file });
    report.screenshots.push({ name, ...requireVisibleCanvas(PNG.sync.read(png)) });
  } else { await page.screenshot({ path: file }); report.screenshots.push({ name }); }
}
(async () => {
  try {
    stage('inputs and browser launch');
    requireBrowserInputs(root, readbackRoot, options.swiftshader);
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
    const browserArgs = browserLaunchArgs(options);
    report.arguments = browserArgs;
    report.playwright = playwrightRequire('playwright/package.json').version;
    // The GPU process loads the system Vulkan loader, which finds no driver on a runner without a Vulkan ICD (the loader log
    // says "Found no files"): ANGLE then fails VK_KHR_surface, the compositor falls back, and the WebGPU swap chain texture
    // cannot be created, which destroys the device. Point the loader at the SwiftShader ICD that ships with the browser.
    const launchEnv = { ...process.env };
    if (options.swiftshader && !launchEnv.VK_ICD_FILENAMES) {
      const executable = process.env.N2_BROWSER_EXECUTABLE || chromium.executablePath();
      const icd = path.join(path.dirname(executable), 'vk_swiftshader_icd.json');
      if (fs.existsSync(icd)) launchEnv.VK_ICD_FILENAMES = icd;
    }
    report.vulkanIcd = launchEnv.VK_ICD_FILENAMES || null;
    browser = await chromium.launch({ channel: 'chromium', executablePath: process.env.N2_BROWSER_EXECUTABLE || undefined,
      headless: !options.headed, args: browserArgs, env: launchEnv });
    report.browser = browser.version();
    messages.push('Browser: ' + report.browser + '; Playwright: ' + report.playwright + '; platform: ' + process.platform);
    page = await browser.newPage({ viewport: { width: 800, height: 450 }, deviceScaleFactor: 2 });
    await page.context().tracing.start({ screenshots: true, snapshots: true });
    page.on('console', msg => { messages.push(msg.type() + ': ' + msg.text()); });
    page.on('pageerror', error => { report.pageErrors.push(String(error)); messages.push('PAGE ERROR: ' + error); });
    page.on('requestfailed', request => messages.push('REQUEST FAILED: ' + request.url() + ': ' + request.failure()?.errorText));
    await page.addInitScript(() => {
      let api;
      window.__gpuFrames = 0;
      window.__gpuEvents = [];
      Object.defineProperty(window, 'N2Loader', { get: () => api, set(value) {
        api = value;
        const session = api.createGpuSession;
        api.createGpuSession = function (options) {
          window.__gpuDevice = options.device;
          const onEvent = options.onEvent;
          options.onEvent = function (text) { window.__gpuEvents.push(text); if (onEvent) onEvent(text); };
          const result = session(options);
          window.__gpuSession = result;
          const present = result.present;
          result.present = function (texture) { present(texture); window.__gpuFrames++; };
          return result;
        };
      } });
    });
    const base = 'http://127.0.0.1:' + server.address().port;
    stage('WebGPU selection and presentation');
    await page.goto(base + '/');
    await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    assert.equal(await page.evaluate(() => !!window.__gpuSession), true, 'WebGPU must acquire a real device');
    report.adapter = await page.evaluate(async () => {
      const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      const info = adapter && adapter.info;
      return info ? { vendor: info.vendor, architecture: info.architecture, device: info.device,
        description: info.description, fallback: info.isFallbackAdapter ?? adapter.isFallbackAdapter } : null;
    });
    messages.push('Adapter: ' + JSON.stringify(report.adapter));
    if (options.swiftshader) requireSwiftShader(report.adapter);
    await page.click('#start');
    await page.waitForFunction(() => window.__gpuFrames >= 10, null, { timeout: 30000 });
    assert.equal(await page.locator('#error').isVisible(), false, messages.join('\n'));
    assert.deepEqual(await page.locator('#canvas').evaluate(c => [c.width, c.height]), [1600, 900]);
    await screenshot('gpu.png', true);
    stage('HiDPI and resize');
    await page.setViewportSize({ width: 1440, height: 810 });
    await page.waitForFunction(() => document.querySelector('#canvas').width === 2560);
    assert.deepEqual(await page.locator('#canvas').evaluate(c => [c.width, c.height]), [2560, 1440]);
    await page.setViewportSize({ width: 800, height: 450 });
    await page.waitForFunction(() => document.querySelector('#canvas').width === 1600);
    // The default project exposes the built-in shaders, text effects, world/UI
    // canvases, materials, models and CPU particles on its numbered stations.
    for (const key of ['2', '3', '4', '5', '6', '7', '8', '0', '-', '5']) {
      const frame = await page.evaluate(() => window.__gpuFrames);
      await page.keyboard.down(key);
      await page.waitForFunction(n => window.__gpuFrames > n + 3, frame);
      await page.keyboard.up(key);
      assert.equal(await page.locator('#error').isVisible(), false);
      await screenshot('station-' + key + '.png', true);
    }
    await page.setViewportSize({ width: 640, height: 360 });
    await page.waitForFunction(() => document.querySelector('#canvas').width === 1280);
    stage('device recovery');
    const oldFrames = await page.evaluate(() => window.__gpuFrames);
    await page.evaluate(() => window.__gpuSession.device.destroy());
    await page.waitForFunction(() => window.__gpuSession.device !== window.__gpuDevice && window.__gpuFrames > 20, null, { timeout: 30000 });
    await page.waitForFunction(n => window.__gpuFrames > n + 10, oldFrames);
    assert.equal(await page.locator('#error').isVisible(), false, messages.join('\n'));
    await screenshot('gpu-restored.png', true);
    for (const key of ['4', '6', '7', '8', '-', '1']) {
      const frame = await page.evaluate(() => window.__gpuFrames);
      await page.keyboard.down(key);
      await page.waitForFunction(n => window.__gpuFrames > n + 3, frame);
      await page.keyboard.up(key);
      await screenshot('restored-station-' + key + '.png', true);
    }
    const events = await page.evaluate(() => window.__gpuEvents);
    assert.deepEqual(events.filter(x => !x.startsWith('Restoring WebGPU:')), [], events.join('\n'));
    messages.push('GPU events: ' + events.join('; '));
    await page.evaluate(() => window.__gpuSession.device.destroy());
    stage('exhausted recovery and software fallback');
    await page.getByRole('button', { name: 'Continue with the software renderer' }).waitFor();
    await page.getByRole('button', { name: 'Continue with the software renderer' }).click();
    await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    assert.match(page.url(), /renderer=software/);
    assert.equal(await page.evaluate(() => !!window.__gpuSession), false);
    await page.click('#start');
    await page.waitForFunction(() => document.querySelector('#canvas').width === 640);
    await screenshot('software.png', true);
    // If loss exhausts recovery before Start, replace the canvas and start
    // software in this page, without asking the user to reload.
    await page.goto(base + '/?renderer=webgpu');
    await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    await page.evaluate(() => window.__gpuSession.device.destroy());
    await page.waitForFunction(() => window.__gpuSession.device !== window.__gpuDevice);
    await page.evaluate(() => window.__gpuSession.device.destroy());
    await page.waitForFunction(() => document.querySelector('#canvas').getContext('2d') !== null);
    await page.click('#start');
    await page.waitForFunction(() => document.querySelector('#canvas').width === 640);
    assert.equal(await page.evaluate(() => window.__gpuFrames), 0);
    assert.equal(await page.locator('#error').isVisible(), false);
    await page.goto(base + '/?renderer=webgpu');
    await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    await page.click('#start');
    await page.waitForFunction(() => window.__gpuFrames >= 10);
    await page.keyboard.down('Escape');
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('has ended'));
    await page.keyboard.up('Escape');
    assert.equal(await page.evaluate(() => window.__gpuSession.status()), 0);
    assert.deepEqual(await page.evaluate(() => window.__gpuEvents), []);
    // A second localhost hostname is a different secure origin, exercising the
    // iframe path without certificate exceptions or a change to WebGPU flags.
    stage('cross-origin iframe');
    await page.goto(base + '/embed.html');
    const child = page.frames().find(frame => frame.url().startsWith('http://localhost:'));
    assert.ok(child, 'cross-origin player frame must load');
    await child.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    assert.equal(await child.evaluate(() => isSecureContext && !!window.__gpuSession), true);
    await child.click('#start');
    await child.waitForFunction(() => window.__gpuFrames >= 10);
    assert.deepEqual(await child.evaluate(() => window.__gpuEvents), []);
    assert.equal(await child.locator('#error').isVisible(), false);
    await page.screenshot({ path: path.join(evidenceDir, 'iframe.png') });
    // This hostname resolves to our loopback server but is not a trusted
    // localhost origin. Verify the actual browser security gate, not a mock.
    stage('insecure origin and ancestor fallback');
    await page.goto('http://n2-insecure.test:' + server.address().port + '/?renderer=webgpu');
    await page.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    assert.equal(await page.evaluate(() => isSecureContext), false);
    assert.equal(await page.evaluate(() => !!window.__gpuSession), false);
    await page.click('#start');
    await page.waitForFunction(() => document.querySelector('#canvas').width === 640);
    assert.equal(await page.locator('#error').isVisible(), false);
    assert.equal(await page.evaluate(() => window.__gpuFrames), 0);
    await page.screenshot({ path: path.join(evidenceDir, 'insecure-software.png') });
    await page.goto('http://n2-insecure.test:' + server.address().port + '/embed.html');
    const insecureChild = page.frames().find(frame => frame.url().startsWith('http://localhost:'));
    assert.ok(insecureChild);
    await insecureChild.waitForFunction(() => !document.querySelector('#start').disabled, null, { timeout: 120000 });
    assert.equal(await insecureChild.evaluate(() => isSecureContext), false, 'insecure ancestor must gate WebGPU');
    assert.equal(await insecureChild.evaluate(() => !!window.__gpuSession), false);
    const cssWidth = await insecureChild.locator('#canvas').evaluate(c => Math.round(c.getBoundingClientRect().width));
    await insecureChild.click('#start');
    await insecureChild.waitForFunction(width => document.querySelector('#canvas').width === width, cssWidth);
    assert.equal(await insecureChild.locator('#error').isVisible(), false);
    await page.screenshot({ path: path.join(evidenceDir, 'insecure-iframe-software.png') });
    if (readbackRoot) {
      stage('deterministic asynchronous WebGPU pixel goldens');
      await page.goto(base + '/readback-tests.html');
      await page.waitForFunction(() => window.__readbackDone, null, { timeout: 30000 });
      const result = await page.evaluate(() => window.__readbackDone);
      assert.equal(result.status, 0, result.message);
      assert.deepEqual(await page.evaluate(() => window.__readbackErrors), []);
      report.readback = result;
      messages.push('Readback result: ' + JSON.stringify(result));
    }
    assert.deepEqual(report.pageErrors, [], messages.join('\n'));
    report.status = 'passed';
    await page.context().tracing.stop();
    console.log(browser.version() + ': Browser smoke OK: WebGPU, visible pixels, HiDPI cap, resize, recovery, quit, iframe, insecure fallback, readback');
  } catch (error) {
    report.status = 'failed'; report.error = String(error.stack || error);
    messages.push(report.error);
    console.error(report.error);
    if (page) {
      await page.screenshot({ path: path.join(evidenceDir, 'failure.png'), timeout: 5000 }).catch(() => {});
      const body = await page.locator('body').innerText({ timeout: 5000 }).catch(() => '(page unavailable)');
      messages.push('LAST PAGE: ' + body);
      await page.context().tracing.stop({ path: path.join(evidenceDir, 'failure-trace.zip') }).catch(() => {});
    }
    throw error;
  } finally {
    writeEvidence();
    if (browser) await browser.close();
  }
})().catch(() => { process.exitCode = 1; }).finally(() => server.close());
