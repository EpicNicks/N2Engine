#!/usr/bin/env node
// n2shader: the cook tool of the user shaders (orchestrator/proposals/user-shaders.md, stage 1). Turns a .n2shader file
// (JSON header + GLSL fragment stage) into SPIR-V (Vulkan), WGSL (WebGPU, through Tint) and GLSL 330 text (OpenGL), checked
// by glslang, spirv-val, a reflection self-check, Tint and naga, with a deterministic manifest.
//
//   node tools/n2shader/n2shader.mjs cook <file.n2shader> [--out <dir>] [--targets vulkan,webgpu,gl] [--relaxed] [--no-lint] [--require-tools]
//   node tools/n2shader/n2shader.mjs check <file.n2shader> [same options]      (cook without writing)
//   node tools/n2shader/n2shader.mjs prelude <file.n2shader> --target <vulkan|webgpu|gl>   (print the generated GLSL)
//   node tools/n2shader/n2shader.mjs corpus <dir> [--require-tools]            (the conformance corpus, expect.json)
//   node tools/n2shader/n2shader.mjs tools                                      (what was found)
//
// Tools are found through GLSLANG, SPIRV_VAL, SPIRV_DIS, SPIRV_CROSS, TINT and NAGA, then PATH, then the Vulkan SDK.
// Exit codes: 0 ok, 1 the shader or corpus was rejected, 2 usage or a missing tool under --require-tools.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { TARGETS } from './lib/contract.mjs';
import { cookShader, formatDiagnostic, writeOutputs } from './lib/cook.mjs';
import { runCorpus } from './lib/corpus.mjs';
import { parseN2Shader } from './lib/parse.mjs';
import { buildTargetSource } from './lib/prelude.mjs';
import { discoverTools, toolVersion, TOOL_NAMES } from './lib/tools.mjs';

class UsageError extends Error {}

export function parseArguments(argv) {
  const options = { command: null, positional: [], out: null, targets: TARGETS, relaxed: false, lint: true, requireTools: false, target: null };
  const args = [...argv];
  const value = (flag) => {
    const next = args.shift();
    if (next === undefined) throw new UsageError(`${flag} needs a value`);
    return next;
  };
  while (args.length > 0) {
    const arg = args.shift();
    if (arg === '--out') options.out = value(arg);
    else if (arg === '--targets') {
      options.targets = value(arg).split(',').filter(Boolean);
      for (const target of options.targets) if (!TARGETS.includes(target)) throw new UsageError(`unknown target "${target}" (${TARGETS.join(', ')})`);
    } else if (arg === '--target') {
      options.target = value(arg);
      if (!TARGETS.includes(options.target)) throw new UsageError(`unknown target "${options.target}" (${TARGETS.join(', ')})`);
    } else if (arg === '--relaxed') options.relaxed = true;
    else if (arg === '--no-lint') options.lint = false;
    else if (arg === '--require-tools') options.requireTools = true;
    else if (arg === '--help' || arg === '-h') options.command = 'help';
    else if (arg.startsWith('--')) throw new UsageError(`unknown option ${arg}`);
    else if (options.command === null) options.command = arg;
    else options.positional.push(arg);
  }
  return options;
}

export function main(argv, { stdout = (text) => process.stdout.write(text), stderr = (text) => process.stderr.write(text) } = {}) {
  let options;
  try {
    options = parseArguments(argv);
    const out = (line) => stdout(`${line}\n`);
    const need = (count, what) => { if (options.positional.length !== count) throw new UsageError(`usage: ${what}`); };
    switch (options.command) {
      case 'cook':
      case 'check': {
        need(1, `n2shader ${options.command} <file.n2shader> [options]`);
        const file = options.positional[0];
        const result = cookShader(fs.readFileSync(file, 'utf8'), {
          path: file.replace(/\\/g, '/'), targets: options.targets, strict: !options.relaxed, lint: options.lint, requireTools: options.requireTools,
        });
        for (const diagnostic of result.diagnostics) (diagnostic.severity === 'error' ? stderr : stdout)(`${formatDiagnostic(diagnostic)}\n`);
        for (const skipped of result.skipped) out(`note: ${skipped.reason}`);
        if (!result.ok) return result.failedStage === 'tools' ? 2 : 1;
        if (options.command === 'cook') {
          const dir = options.out ?? path.join(path.dirname(file), `${path.basename(file, '.n2shader')}.cooked`);
          const written = writeOutputs(result, dir);
          out(`cooked ${file} -> ${dir}: ${written.join(', ')}`);
        } else out(`ok ${file}`);
        return 0;
      }
      case 'prelude': {
        need(1, 'n2shader prelude <file.n2shader> --target <vulkan|webgpu|gl>');
        if (!options.target) throw new UsageError('prelude needs --target');
        const file = options.positional[0];
        const parsed = parseN2Shader(fs.readFileSync(file, 'utf8'), { path: file });
        stdout(buildTargetSource(options.target, parsed, { path: file.replace(/\\/g, '/') }).text);
        return 0;
      }
      case 'corpus': {
        need(1, 'n2shader corpus <dir> [--require-tools]');
        const tools = discoverTools();
        const result = runCorpus(path.resolve(options.positional[0]), { tools, requireTools: options.requireTools, targets: options.targets, log: out });
        out(result.ok
          ? `corpus ok: ${result.counts.positive} positive, ${result.counts.negative} negative expectations met${result.counts.skipped ? `, ${result.counts.skipped} stage(s) skipped (tools not installed)` : ''}`
          : `corpus FAILED: ${result.counts.failed} problem(s)`);
        return result.ok ? 0 : 1;
      }
      case 'tools': {
        for (const [name, file] of Object.entries(discoverTools())) out(`${name.padEnd(11)} ${file ? `${file}  [${toolVersion(name, file)}]` : 'not found'}`);
        return 0;
      }
      case 'help':
      case null:
        stdout(fs.readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n').filter((line) => line.startsWith('//')).slice(0, 13).map((line) => line.replace(/^\/\/ ?/, '')).join('\n') + '\n');
        return options.command === null ? 2 : 0;
      default:
        throw new UsageError(`unknown command "${options.command}" (${['cook', 'check', 'prelude', 'corpus', 'tools'].join(', ')})`);
    }
  } catch (error) {
    if (error instanceof UsageError) { stderr(`n2shader: ${error.message}\n`); return 2; }
    if (error?.code === 'ENOENT') { stderr(`n2shader: ${error.message}\n`); return 2; }
    throw error;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exitCode = main(process.argv.slice(2));
}

export { TOOL_NAMES };
