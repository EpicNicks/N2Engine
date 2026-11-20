// Authored WGSL (stage 9f, orchestrator/proposals/user-shaders.md section 26): for a shader whose header says
// "targets": { "webgpu": "authored" } the WebGPU payload is the file's own `#stage wgsl` text instead of the WGSL Tint makes from the
// GLSL. The text still has to be a module the engine's WebGPU backend can drive: this file is the contract's check of it (the
// resources, the one fragment entry point and its inputs). Tint (parse, resolve, uniformity, its SPIR-V writer) and naga then
// validate it the way they validate a generated module, and checkWgslLayout (reflect.mjs) compares the Material struct and the
// texture bindings with the header.

import { PER_DRAW, SAMPLER_BINDING_BASE, TEXTURE_BINDING_BASE, VARYINGS, isTextureType } from './contract.mjs';

/** The WGSL type of each varying, as the engine's vertex stage writes it (renderer/shaders/webgpu/user.wgsl) */
const VARYING_TYPES = Object.freeze({ n2_UV: 'vec2<f32>', n2_Color: 'vec4<f32>', n2_WorldPos: 'vec3<f32>', n2_Normal: 'vec3<f32>', n2_Tangent: 'vec4<f32>' });
const PER_DRAW_TYPES = Object.freeze({ mat4: 'mat4x4<f32>', float: 'f32', int: 'i32', vec3: 'vec3<f32>' });

/** The module's size limit (the engine's own: renderer/include/renderer/common/UserShader.hpp kUserShaderMaxWgslBytes) */
export const MAX_WGSL_BYTES = 256 * 1024;

const compact = (type) => type.replace(/\s+/g, '').replace(/,/g, ', ');

/** Splits a parameter list at the commas outside angle brackets and parentheses */
function splitParameters(text) {
  const out = [];
  let depth = 0;
  let current = '';
  for (const char of text) {
    if (char === '<' || char === '(') depth += 1;
    if (char === '>' || char === ')') depth -= 1;
    if (char === ',' && depth === 0) { out.push(current); current = ''; } else current += char;
  }
  if (current.trim() !== '') out.push(current);
  return out.map((entry) => entry.trim()).filter(Boolean);
}

/** The parameter text and return type of `@fragment fn main(...) -> type {`, with the parentheses of attributes balanced; null when it cannot be read */
function readSignature(code) {
  const start = /@fragment\s+fn\s+main\s*\(/.exec(code);
  if (!start) return null;
  let depth = 1;
  let index = start.index + start[0].length;
  const begin = index;
  while (index < code.length && depth > 0) {
    if (code[index] === '(') depth += 1;
    if (code[index] === ')') depth -= 1;
    index += 1;
  }
  if (depth !== 0) return null;
  const rest = /^\s*->\s*([^{]+)\{/.exec(code.slice(index));
  return rest ? [null, code.slice(begin, index - 1), rest[1]] : null;
}

/**
 * The contract's checks of an authored module. Returns diagnostics ({ stage: 'wgsl-authored', ... }); none means it is a module
 * the backend can drive.
 * @param {string} wgsl LF-separated text
 * @param {{ header: { uses: string[], params: object[] }, layout: { members: object[] } }} context
 */
export function checkAuthoredWgsl(wgsl, { header, layout }) {
  const out = [];
  const fail = (message) => out.push({ stage: 'wgsl-authored', severity: 'error', file: null, line: null, message });
  if (Buffer.byteLength(wgsl, 'utf8') > MAX_WGSL_BYTES) fail(`the WGSL is over ${MAX_WGSL_BYTES / 1024} KiB`);
  if (wgsl.trim() === '') { fail('the "#stage wgsl" block is empty'); return out; }
  // no comments or strings in the way of the scans below
  const code = wgsl.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');

  if (/^\s*(?:enable|requires|diagnostic)\b/m.test(code)) fail('"enable", "requires" and "diagnostic" directives are not allowed: an authored module uses core WGSL only');
  if (/@(?:vertex|compute)\b/.test(code)) fail('an authored module has one fragment entry point (the vertex stage is the engine\'s); @vertex and @compute are not allowed');
  const entries = [...code.matchAll(/@fragment\s+fn\s+(\w+)/g)].map((match) => match[1]);
  if (entries.length !== 1 || entries[0] !== 'main') fail(`the module needs exactly one fragment entry point, "@fragment fn main(...)" (found ${entries.length === 0 ? 'none' : entries.map((name) => `"${name}"`).join(', ')})`);

  // the resources: exactly the contract's bindings for this shader
  const textureSlots = header.params.filter((param) => isTextureType(param.type)).map((param) => param.slot);
  const allowed = new Map([['0/0', 'the PerDraw uniform block']]);
  if (header.uses.includes('lighting')) allowed.set('0/1', 'the Lighting uniform block');
  if (layout.members.length > 0) allowed.set('1/0', 'the Material uniform block');
  for (const slot of textureSlots) {
    allowed.set(`1/${TEXTURE_BINDING_BASE + slot}`, `texture slot ${slot}`);
    allowed.set(`1/${SAMPLER_BINDING_BASE + slot}`, `the sampler of texture slot ${slot}`);
  }
  const bound = new Map();
  const bindingPattern = /@group\(\s*(\d+)u?\s*\)\s*@binding\(\s*(\d+)u?\s*\)\s*var(?:<\s*([\w, ]+?)\s*>)?\s+(\w+)\s*:\s*([^;]+);/g;
  for (const match of code.matchAll(bindingPattern)) {
    const key = `${match[1]}/${match[2]}`;
    if (!allowed.has(key)) { fail(`@group(${match[1]}) @binding(${match[2]}) ("${match[4]}") is not a resource of this shader's contract (allowed: ${[...allowed.keys()].map((entry) => `${entry.replace('/', ',')}`).join('; ')})`); continue; }
    if (bound.has(key)) fail(`@group(${match[1]}) @binding(${match[2]}) is declared twice`);
    bound.set(key, { space: match[3] ?? '', name: match[4], type: match[5].trim() });
  }
  if (/@group\b/.test(code) && bound.size === 0) fail('the @group/@binding declarations could not be read: write each as "@group(g) @binding(b) var<...> name: Type;" on one line');
  for (const [key, entry] of bound) {
    const wantUniform = ['0/0', '0/1', '1/0'].includes(key);
    if (wantUniform && entry.space !== 'uniform') fail(`@group(${key.replace('/', ') @binding(')}) must be "var<uniform>"`);
  }
  if (!bound.has('0/0') && /\bn2_(?:Model|View|Projection)\b/.test(code)) fail('the PerDraw block (@group(0) @binding(0)) is used but not declared');

  // the PerDraw struct: the contract's members, in order (the engine writes them at those offsets)
  const perDraw = bound.get('0/0');
  if (perDraw) {
    const struct = new RegExp(`struct\\s+${perDraw.type}\\s*\\{([^}]*)\\}`).exec(code);
    if (!struct) fail(`the PerDraw block's type "${perDraw.type}" is not a struct declared in the module`);
    else {
      const members = [...struct[1].matchAll(/(?:@\w+(?:\([^)]*\))?\s*)*(\w+)\s*:\s*([^,]+?)\s*(?:,|$)/g)].map((entry) => ({ name: entry[1], type: compact(entry[2]) }));
      PER_DRAW.forEach((want, index) => {
        const got = members[index];
        if (!got || got.name !== want.name || got.type !== PER_DRAW_TYPES[want.type]) fail(`PerDraw member ${index} must be "${want.name}: ${PER_DRAW_TYPES[want.type]}" (the engine writes the block in the contract's order)`);
      });
      if (members.length !== PER_DRAW.length) fail(`PerDraw has ${members.length} members, the contract has ${PER_DRAW.length}`);
    }
  }

  // the entry point's inputs and output
  const signature = readSignature(code);
  if (signature) {
    const seen = new Set();
    for (const parameter of splitParameters(signature[1])) {
      const location = /^@location\(\s*(\d+)u?\s*\)\s*(?:@\w+(?:\([^)]*\))?\s*)*(\w+)\s*:\s*(.+)$/s.exec(parameter);
      const builtin = /^@builtin\(\s*position\s*\)\s*(\w+)\s*:\s*(.+)$/s.exec(parameter);
      if (builtin) {
        if (compact(builtin[2]) !== 'vec4<f32>') fail(`@builtin(position) must be a vec4<f32> (found ${compact(builtin[2])})`);
        continue;
      }
      if (!location) { fail(`the fragment input "${parameter.replace(/\s+/g, ' ')}" is not a "@location(n) name: type" or "@builtin(position)" parameter: write the inputs as parameters of main, not as a struct`); continue; }
      const index = Number(location[1]);
      const varying = VARYINGS.find((entry) => entry.location === index);
      if (!varying) { fail(`@location(${index}) is not one of the engine's varyings (0 to ${VARYINGS.length - 1})`); continue; }
      if (compact(location[3]) !== VARYING_TYPES[varying.name]) fail(`@location(${index}) is ${varying.name}, a ${VARYING_TYPES[varying.name]}; found ${compact(location[3])}`);
      if (seen.has(index)) fail(`@location(${index}) is read twice`);
      seen.add(index);
    }
    if (!/^@location\(\s*0u?\s*\)\s*vec4<f32>\s*$/.test(signature[2].trim())) fail(`main must return "@location(0) vec4<f32>" (found ${JSON.stringify(signature[2].trim())})`);
  } else if (entries.length === 1) fail('the signature of "main" could not be read: write "@fragment fn main(params) -> @location(0) vec4<f32> {"');
  return out;
}
