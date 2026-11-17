// The cook: parse, lint, generate the per-target GLSL, then glslang -> spirv-val -> reflection self-check -> Tint -> naga, and
// the manifest. Every external stage is skipped, with a message, when its tool is not installed (requireTools turns the
// skip into an error). Nothing here is random or time dependent: the same input and tools give the same bytes.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {
  CONTRACT_VERSION, FORMAT_VERSION, SAMPLER_BINDING_BASE, TARGETS, TEXTURE_BINDING_BASE, VERTEX_ATTRIBUTES,
} from './contract.mjs';
import { lintStage } from './lint.mjs';
import { ShaderError, parseN2Shader } from './parse.mjs';
import { buildTargetSource, PRELUDE_DIR } from './prelude.mjs';
import { checkCapabilities, checkReflection, checkWgslLayout } from './reflect.mjs';
import { layoutBlock } from './std140.mjs';
import { discoverTools, run, sha256, toolVersion } from './tools.mjs';

const PAYLOAD_FILES = { vulkan: 'shader.vulkan.frag.spv', webgpu: 'shader.webgpu.wgsl', gl: 'shader.gl.frag' };
export const MANIFEST_FILE = 'manifest.json';

const LINE_PATTERN = /^(ERROR|WARNING):\s*(.+?):(\d+):\s*(.*)$/;

/** glslang's output as diagnostics, with the generated file's lines mapped to the author's (#line) or to the prelude */
export function parseGlslangOutput(output, { target, userPath, inputName }) {
  const out = [];
  for (const raw of output.split(/\r?\n/)) {
    const text = raw.trim();
    if (!/^(ERROR|WARNING):/.test(text)) continue;
    const severity = text.startsWith('ERROR') ? 'error' : 'warning';
    const match = LINE_PATTERN.exec(text);
    if (!match) {
      if (/compilation (?:error|terminated)|Missing entry point/i.test(text)) continue; // the summary lines add nothing
      out.push({ stage: 'glslang', severity, file: null, line: null, message: `${target}: ${text.replace(/^(ERROR|WARNING):\s*/, '')}` });
      continue;
    }
    const [, , where, lineText, message] = match;
    const line = Number(lineText);
    const isUser = where === userPath || (target === 'gl' && where === '1');
    if (message === '' || /^compilation terminated/.test(message.trim())) continue;
    out.push({
      stage: 'glslang',
      severity,
      file: isUser ? userPath : `prelude:${target}`,
      line,
      message: isUser ? message : `${message} (in the engine's generated ${target} prelude, generated line ${line}: ${where === inputName || where === '0' ? 'not the author\'s code' : where})`,
    });
  }
  return out;
}

/** Tint's output reduced to its error lines (it prints the offending source line and a caret under each) */
function tintMessages(output) {
  return output.split(/\r?\n/).map((line) => line.trimEnd()).filter((line) => line.trim() !== '');
}

function summarise(output, limit = 8) {
  const lines = tintMessages(output);
  return lines.slice(0, limit).join(' | ');
}

const hasWarning = (log) => /(?:^|[\s:])warning:/im.test(log);

/**
 * Cooks one .n2shader.
 * @param {string} text the file's contents
 * @param {object} [options]
 * @param {string} [options.path] the path cited in diagnostics and #line (default shader.n2shader)
 * @param {string[]} [options.targets] subset of vulkan, webgpu, gl
 * @param {object} [options.tools] discoverTools() result (default: discovered)
 * @param {boolean} [options.strict] the strict lint mode (default true; false = --relaxed: while/do loops allowed)
 * @param {boolean} [options.lint] run the lint (default true)
 * @param {boolean} [options.requireTools] a missing tool is an error instead of a skipped stage
 * @param {string} [options.preludeDir] the prelude templates (default renderer/shaders/user/prelude)
 * @returns {{ ok: boolean, failedStage: string|null, diagnostics: object[], skipped: object[], outputs: object, manifest: object|null }}
 */
export function cookShader(text, options = {}) {
  const {
    path: file = 'shader.n2shader', targets = TARGETS, strict = true, lint = true, requireTools = false, preludeDir = PRELUDE_DIR,
  } = options;
  const tools = options.tools ?? discoverTools();
  const result = { ok: false, failedStage: null, diagnostics: [], skipped: [], outputs: {}, manifest: null };
  const add = (diagnostic) => {
    result.diagnostics.push(diagnostic);
    if (diagnostic.severity === 'error' && result.failedStage === null) result.failedStage = diagnostic.stage;
  };

  let parsed;
  try {
    parsed = parseN2Shader(text, { path: file });
  } catch (error) {
    if (!(error instanceof ShaderError)) throw error;
    add({ stage: 'parse', severity: 'error', file: error.file, line: error.line, message: error.message });
    return result;
  }
  const { header } = parsed;

  if (lint) {
    for (const entry of lintStage(parsed.source, { firstLine: parsed.firstLine, header, strict })) {
      add({ stage: 'lint', severity: entry.severity, file, line: entry.line, message: entry.message, rule: entry.rule });
    }
    if (result.failedStage !== null) return result;
  }

  const layout = layoutBlock(header.params);
  const used = new Set();
  const passed = {};
  const skip = (stage, target, tool) => {
    const reason = `${stage} skipped for ${target}: ${tool} is not installed`;
    if (requireTools) add({ stage: 'tools', severity: 'error', file: null, line: null, message: reason });
    else result.skipped.push({ stage, target, reason });
  };

  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'n2shader-'));
  try {
    for (const target of targets) {
      passed[target] = [];
      const generated = buildTargetSource(target, parsed, { path: file, dir: preludeDir });
      const source = path.join(work, `${target}.frag`);
      fs.writeFileSync(source, generated.text);
      const inputName = path.basename(source);
      result.outputs[target] = { file: PAYLOAD_FILES[target], data: null };

      if (target === 'gl') {
        result.outputs.gl.data = Buffer.from(generated.text);
        if (!tools.glslang) { skip('glslang', target, 'glslangValidator'); continue; }
        used.add('glslang');
        const out = run(tools.glslang, ['-S', 'frag', source]);
        const diagnostics = parseGlslangOutput(`${out.stdout}\n${out.stderr}`, { target, userPath: file, inputName });
        diagnostics.forEach(add);
        if (out.status !== 0 && !diagnostics.some((entry) => entry.severity === 'error')) add({ stage: 'glslang', severity: 'error', file: null, line: null, message: `gl: glslang exited with ${out.status}: ${summarise(out.stdout + out.stderr)}` });
        if (out.status === 0) passed.gl.push('glslang');
        continue;
      }

      // ---- glslang: GLSL 450 -> SPIR-V
      if (!tools.glslang) { skip('glslang', target, 'glslangValidator'); continue; }
      used.add('glslang');
      const spv = path.join(work, `${target}.spv`);
      const compiled = run(tools.glslang, ['-V', '--target-env', 'vulkan1.1', '-S', 'frag', '-o', spv, source]);
      const compileDiagnostics = parseGlslangOutput(`${compiled.stdout}\n${compiled.stderr}`, { target, userPath: file, inputName });
      compileDiagnostics.forEach(add);
      if (compiled.status !== 0) {
        if (!compileDiagnostics.some((entry) => entry.severity === 'error')) add({ stage: 'glslang', severity: 'error', file: null, line: null, message: `${target}: glslang exited with ${compiled.status}: ${summarise(compiled.stdout + compiled.stderr)}` });
        continue;
      }
      passed[target].push('glslang');
      const spirv = fs.readFileSync(spv);

      // ---- spirv-val
      if (!tools.spirvVal) skip('spirv-val', target, 'spirv-val');
      else {
        used.add('spirvVal');
        const valid = run(tools.spirvVal, ['--target-env', 'vulkan1.1', spv]);
        if (valid.status !== 0) { add({ stage: 'spirv-val', severity: 'error', file: null, line: null, message: `${target}: ${summarise(valid.stdout + valid.stderr)}` }); continue; }
        passed[target].push('spirv-val');
      }

      // ---- capability allow-list
      if (!tools.spirvDis) skip('spirv-dis', target, 'spirv-dis');
      else {
        used.add('spirvDis');
        const disassembly = run(tools.spirvDis, [spv]);
        const capabilityDiagnostics = checkCapabilities(disassembly.stdout, target);
        capabilityDiagnostics.forEach(add);
        if (capabilityDiagnostics.length > 0) continue;
        passed[target].push('capabilities');
      }

      // ---- reflection self-check
      if (!tools.spirvCross) skip('reflect', target, 'spirv-cross');
      else {
        used.add('spirvCross');
        const reflected = run(tools.spirvCross, [spv, '--reflect']);
        let json = null;
        try { json = JSON.parse(reflected.stdout); } catch { /* reported below */ }
        if (reflected.status !== 0 || json === null) { add({ stage: 'reflect', severity: 'error', file: null, line: null, message: `${target}: spirv-cross --reflect failed: ${summarise(reflected.stdout + reflected.stderr)}` }); continue; }
        const reflectDiagnostics = checkReflection(json, { target, header, layout });
        reflectDiagnostics.forEach(add);
        if (reflectDiagnostics.length > 0) continue;
        passed[target].push('reflect');
      }

      if (target === 'vulkan') { result.outputs.vulkan.data = spirv; continue; }

      // ---- Tint: SPIR-V -> WGSL (the translator; naga only validates)
      if (!tools.tint) { skip('tint', target, 'tint'); continue; }
      used.add('tint');
      const wgslPath = path.join(work, 'webgpu.wgsl');
      const translated = run(tools.tint, ['--format', 'wgsl', '-o', wgslPath, spv]);
      const translatedLog = `${translated.stdout}\n${translated.stderr}`;
      if (translated.status !== 0 || !fs.existsSync(wgslPath)) {
        // "Failed to parse SPIR-V: <reason>" means the reader exists and rejected the module; the hint is for a build without one
        const reader = /spir-?v reader[^\n]*(?:not|disabled)|(?:not (?:compiled|built|enabled)|without)[^\n]*spir-?v|unknown input format/i.test(translatedLog)
          ? ' (this Tint build seems to have no SPIR-V reader)' : '';
        add({ stage: 'tint', severity: 'error', file: null, line: null, message: `${target}: tint failed${reader}: ${summarise(translatedLog)}` });
        continue;
      }
      if (hasWarning(translatedLog)) { add({ stage: 'tint', severity: 'error', file: null, line: null, message: `${target}: tint printed a warning (warnings are errors): ${summarise(translatedLog)}` }); continue; }
      const wgsl = fs.readFileSync(wgslPath, 'utf8');
      passed.webgpu.push('tint');
      // the WGSL read back by Tint: parse, resolve and uniformity again, then its SPIR-V writer with spirv-val
      const roundTrip = run(tools.tint, ['--format', 'spvasm', '--validate', '--ep', 'main', '-o', path.join(work, 'roundtrip.spvasm'), wgslPath]);
      if (roundTrip.status !== 0 || hasWarning(`${roundTrip.stdout}\n${roundTrip.stderr}`)) { add({ stage: 'tint', severity: 'error', file: null, line: null, message: `webgpu: tint rejected its own WGSL: ${summarise(roundTrip.stdout + roundTrip.stderr)}` }); continue; }
      passed.webgpu.push('tint-roundtrip');
      const layoutDiagnostics = checkWgslLayout(wgsl, { header, layout });
      layoutDiagnostics.forEach(add);
      if (layoutDiagnostics.length > 0) continue;
      passed.webgpu.push('wgsl-layout');
      result.outputs.webgpu.data = Buffer.from(wgsl);

      // ---- naga: a second opinion (it reads the WGSL, it cannot read this SPIR-V's combined-sampler form)
      if (!tools.naga) { skip('naga', target, 'naga'); continue; }
      used.add('naga');
      const checked = run(tools.naga, [wgslPath]);
      const checkedLog = `${checked.stdout}\n${checked.stderr}`;
      if (checked.status !== 0) { add({ stage: 'naga', severity: 'error', file: null, line: null, message: `webgpu: naga rejected the WGSL: ${summarise(checkedLog)}` }); continue; }
      if (hasWarning(checkedLog)) { add({ stage: 'naga', severity: 'error', file: null, line: null, message: `webgpu: naga printed a warning: ${summarise(checkedLog)}` }); continue; }
      passed.webgpu.push('naga');
    }
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }

  if (result.failedStage !== null) {
    result.outputs = {};
    return result;
  }
  result.ok = true;
  result.manifest = buildManifest({ text, file, header, layout, targets, outputs: result.outputs, passed, tools, used, strict, lint });
  return result;
}

function buildManifest({ text, file, header, layout, targets, outputs, passed, tools, used, strict, lint }) {
  const members = new Map(layout.members.map((member) => [member.name, member]));
  const params = header.params.map((param) => {
    const entry = { name: param.name, type: param.type };
    if (param.type === 'texture') entry.slot = param.slot;
    else { entry.offset = members.get(param.name).offset; entry.size = members.get(param.name).size; }
    entry.default = param.default;
    for (const key of ['min', 'max', 'step', 'label', 'tooltip', 'group']) if (param[key] !== undefined) entry[key] = param[key];
    return entry;
  });
  const textures = header.params.filter((param) => param.type === 'texture').sort((a, b) => a.slot - b.slot).map((param) => ({
    name: param.name, slot: param.slot, default: param.default, binding: TEXTURE_BINDING_BASE + param.slot, samplerBinding: SAMPLER_BINDING_BASE + param.slot,
  }));
  const payloads = {};
  for (const target of targets) {
    const output = outputs[target];
    if (output?.data) payloads[target] = { file: output.file, bytes: output.data.length, sha256: sha256(output.data) };
  }
  const toolVersions = {};
  for (const name of [...used].sort()) toolVersions[name] = toolVersion(name, tools[name]);
  return {
    formatVersion: FORMAT_VERSION,
    contract: CONTRACT_VERSION,
    name: header.name,
    source: { file: path.basename(file), sha256: sha256(text.replace(/\r\n/g, '\n')) },
    uses: header.uses,
    state: header.state,
    blockSize: layout.size,
    params,
    textures,
    vertexAttributes: [...VERTEX_ATTRIBUTES],
    lint: lint ? (strict ? 'strict' : 'relaxed') : 'off',
    payloads,
    validated: Object.fromEntries(targets.map((target) => [target, passed[target] ?? []])),
    tools: toolVersions,
  };
}

/** The manifest as bytes: fixed key order, two-space indent, one trailing newline */
export function manifestBytes(manifest) {
  return Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`);
}

/** Writes manifest.json and the payloads of a successful cook into `dir` (sorted, no timestamps) */
export function writeOutputs(result, dir) {
  if (!result.ok) throw new Error('writeOutputs: the cook failed');
  fs.mkdirSync(dir, { recursive: true });
  const written = [];
  for (const target of Object.keys(result.outputs).sort()) {
    const output = result.outputs[target];
    if (!output?.data) continue;
    fs.writeFileSync(path.join(dir, output.file), output.data);
    written.push(output.file);
  }
  fs.writeFileSync(path.join(dir, MANIFEST_FILE), manifestBytes(result.manifest));
  written.push(MANIFEST_FILE);
  return written.sort();
}

/** One line per diagnostic, "file:line: severity: message" (or "stage: severity: message" with no location) */
export function formatDiagnostic(diagnostic) {
  const where = diagnostic.file && diagnostic.line ? `${diagnostic.file}:${diagnostic.line}` : (diagnostic.file ?? diagnostic.stage);
  return `${where}: ${diagnostic.severity}: [${diagnostic.stage}] ${diagnostic.message}`;
}
