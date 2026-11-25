// The .n2shader parser: a strict-JSON header, then everything after the line "#stage fragment" is GLSL (proposal section
// 3.1). The header is validated completely here, so the later stages work on a normalised, known-good description.
import { LIMITS, FORMAT_VERSION, PARAM_TYPES, TEXTURE_DEFAULTS, BLEND_MODES, CULL_MODES, QUEUES, USES, RESERVED_PARAMS, DEPTH_RANGE, isTextureType } from './contract.mjs';
import { layoutBlock } from './std140.mjs';

export class ShaderError extends Error {
  constructor(stage, message, { file = null, line = null, rule = null } = {}) {
    super(message);
    this.name = 'ShaderError';
    this.stage = stage;
    this.file = file;
    this.line = line;
    this.rule = rule;
  }
}

const TOP_KEYS = ['n2shader', 'name', 'params', 'uses', 'state', 'targets', 'fallback'];
/** "fallback" (issue #445, read by the engine only): what stands in where the shader cannot be drawn */
const FALLBACK_KEYS = ['tint', 'opacity', 'hidden'];
const PARAM_KEYS = ['name', 'type', 'default', 'min', 'max', 'step', 'label', 'tooltip', 'group', 'depth'];
const STATE_KEYS = ['blend', 'cull', 'depthWrite', 'queue'];
const IDENTIFIER = /^[A-Za-z_][A-Za-z0-9_]*$/;
// GLSL keywords that cannot be a member name (the short list a parameter name could plausibly hit)
const GLSL_KEYWORDS = new Set([
  'void', 'bool', 'int', 'uint', 'float', 'double', 'vec2', 'vec3', 'vec4', 'ivec2', 'ivec3', 'ivec4', 'uvec2', 'uvec3', 'uvec4',
  'bvec2', 'bvec3', 'bvec4', 'mat2', 'mat3', 'mat4', 'sampler2D', 'texture2D', 'sampler', 'struct', 'const', 'uniform', 'buffer',
  'in', 'out', 'inout', 'layout', 'if', 'else', 'for', 'while', 'do', 'switch', 'case', 'default', 'break', 'continue', 'return',
  'discard', 'true', 'false', 'main', 'highp', 'mediump', 'lowp', 'precision', 'flat', 'smooth', 'shared', 'attribute', 'varying',
]);

function depthOf(value) {
  if (value === null || typeof value !== 'object') return 0;
  let deepest = 0;
  for (const child of Array.isArray(value) ? value : Object.values(value)) deepest = Math.max(deepest, depthOf(child));
  return deepest + 1;
}

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

function defaultFor(type) {
  switch (type) {
    case 'float': return 0;
    case 'int': return 0;
    case 'bool': return false;
    case 'vec2': return [0, 0];
    case 'vec3': return [0, 0, 0];
    case 'vec4': return [0, 0, 0, 0];
    case 'color': return [1, 1, 1, 1];
    case 'mat3': return [1, 0, 0, 0, 1, 0, 0, 0, 1];
    case 'mat4': return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    default: return 'white';
  }
}

const VECTOR_SIZES = { vec2: 2, vec3: 3, vec4: 4, color: 4 };

function checkDefault(param, fail) {
  const { type } = param;
  const value = param.default;
  if (type === 'float' && !isNumber(value)) fail(`parameter "${param.name}": a float default must be a number`);
  if (type === 'int' && !Number.isInteger(value)) fail(`parameter "${param.name}": an int default must be an integer`);
  if (type === 'bool' && typeof value !== 'boolean') fail(`parameter "${param.name}": a bool default must be true or false`);
  if (VECTOR_SIZES[type] !== undefined) {
    if (!Array.isArray(value) || value.length !== VECTOR_SIZES[type] || !value.every(isNumber)) {
      fail(`parameter "${param.name}": a ${type} default must be an array of ${VECTOR_SIZES[type]} numbers`);
    }
  }
  if ((type === 'mat3' || type === 'mat4') && param.hasDefault) {
    fail(`parameter "${param.name}": a ${type} starts as the identity matrix and takes no "default" (set it from a Material, a .mat file or a script)`);
  }
  if (isTextureType(type) && !TEXTURE_DEFAULTS.includes(value)) {
    fail(`parameter "${param.name}": a ${type} default must be one of ${TEXTURE_DEFAULTS.join(', ')}`);
  }
}

/**
 * Validates and normalises the header object. `fail(message)` throws a ShaderError.
 * @returns {{ name: string, params: object[], uses: string[], state: object, targets: object }}
 */
export function validateHeader(raw, fail) {
  if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) fail('the header must be a JSON object');
  if (depthOf(raw) > LIMITS.headerDepth) fail(`the header is nested deeper than ${LIMITS.headerDepth} levels`);
  for (const key of Object.keys(raw)) {
    if (!TOP_KEYS.includes(key)) fail(`unknown header key "${key}" (known: ${TOP_KEYS.join(', ')})`);
  }
  if (!Number.isInteger(raw.n2shader)) fail('the header needs "n2shader": <format version>');
  if (raw.n2shader > FORMAT_VERSION) {
    fail(`this file is format ${raw.n2shader}, newer than the format ${FORMAT_VERSION} this tool reads; update the tool`);
  }
  if (raw.n2shader < 1) fail(`unsupported format version ${raw.n2shader}`);
  if (typeof raw.name !== 'string' || raw.name.trim() === '') fail('the header needs a non-empty "name"');

  const paramsRaw = raw.params ?? [];
  if (!Array.isArray(paramsRaw)) fail('"params" must be an array');
  if (paramsRaw.length > LIMITS.params) fail(`at most ${LIMITS.params} parameters (found ${paramsRaw.length})`);
  const seen = new Set();
  const params = [];
  for (const [index, entry] of paramsRaw.entries()) {
    if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) fail(`params[${index}] must be an object`);
    for (const key of Object.keys(entry)) {
      if (!PARAM_KEYS.includes(key)) fail(`params[${index}]: unknown key "${key}" (known: ${PARAM_KEYS.join(', ')})`);
    }
    const { name, type } = entry;
    if (typeof name !== 'string' || !IDENTIFIER.test(name)) fail(`params[${index}]: "name" must be an identifier`);
    if (name.startsWith('n2_') || name.startsWith('gl_')) fail(`parameter "${name}": names starting with n2_ or gl_ are reserved`);
    if (name.includes('__')) fail(`parameter "${name}": a name must not contain a double underscore`);
    if (GLSL_KEYWORDS.has(name)) fail(`parameter "${name}": a GLSL keyword cannot be a parameter name`);
    if (seen.has(name)) fail(`parameter "${name}" is declared twice`);
    seen.add(name);
    if (!PARAM_TYPES.includes(type)) fail(`parameter "${name}": type must be one of ${PARAM_TYPES.join(', ')} (found ${JSON.stringify(type)})`);
    if (RESERVED_PARAMS[name] !== undefined && RESERVED_PARAMS[name] !== type) {
      fail(`parameter "${name}" is reserved for the standard Material field and must be a ${RESERVED_PARAMS[name]}`);
    }
    const param = { name, type, default: entry.default === undefined ? defaultFor(type) : entry.default };
    param.hasDefault = entry.default !== undefined;
    checkDefault(param, fail);
    delete param.hasDefault;
    for (const key of ['min', 'max', 'step']) {
      if (entry[key] === undefined) continue;
      if (type !== 'float' && type !== 'int') fail(`parameter "${name}": "${key}" applies to float and int only`);
      if (!isNumber(entry[key])) fail(`parameter "${name}": "${key}" must be a number`);
      param[key] = entry[key];
    }
    if (param.min !== undefined && param.max !== undefined && param.min > param.max) fail(`parameter "${name}": min is above max`);
    if (type === 'texture3d') {
      if (!Number.isInteger(entry.depth) || entry.depth < DEPTH_RANGE.min || entry.depth > DEPTH_RANGE.max) {
        fail(`parameter "${name}": a texture3d needs "depth", the number of slices (an integer from ${DEPTH_RANGE.min} to ${DEPTH_RANGE.max})`);
      }
      param.depth = entry.depth;
    } else if (entry.depth !== undefined) {
      fail(`parameter "${name}": "depth" applies to texture3d only`);
    }
    for (const key of ['label', 'tooltip', 'group']) {
      if (entry[key] === undefined) continue;
      if (typeof entry[key] !== 'string') fail(`parameter "${name}": "${key}" must be a string`);
      param[key] = entry[key];
    }
    params.push(param);
  }

  // Texture slots: declaration order, except that the reserved baseColorTexture takes slot 0 (the Material's base texture)
  for (const param of params) {
    if (param.type === 'texture3d' && seen.has(`${param.name}_depth`)) fail(`parameter "${param.name}_depth" collides with the constant the texture3d "${param.name}" defines`);
  }
  const textures = params.filter((param) => isTextureType(param.type));
  if (textures.length > LIMITS.textures) fail(`at most ${LIMITS.textures} textures (found ${textures.length})`);
  const ordered = [...textures.filter((param) => param.name === 'baseColorTexture'), ...textures.filter((param) => param.name !== 'baseColorTexture')];
  ordered.forEach((param, slot) => { param.slot = slot; });

  const block = layoutBlock(params);
  if (block.size > LIMITS.blockBytes) fail(`the Material block is ${block.size} bytes, over the ${LIMITS.blockBytes}-byte limit`);

  const usesRaw = raw.uses ?? [];
  if (!Array.isArray(usesRaw)) fail('"uses" must be an array');
  for (const use of usesRaw) {
    if (!USES.includes(use)) fail(`unknown "uses" entry ${JSON.stringify(use)} (known: ${USES.join(', ')})`);
  }
  const uses = [...new Set(usesRaw)].sort();

  const stateRaw = raw.state ?? {};
  if (stateRaw === null || typeof stateRaw !== 'object' || Array.isArray(stateRaw)) fail('"state" must be an object');
  for (const key of Object.keys(stateRaw)) {
    if (!STATE_KEYS.includes(key)) fail(`unknown state key "${key}" (known: ${STATE_KEYS.join(', ')})`);
  }
  if (stateRaw.blend !== undefined && !BLEND_MODES.includes(stateRaw.blend)) fail(`state.blend must be one of ${BLEND_MODES.join(', ')}`);
  if (stateRaw.cull !== undefined && !CULL_MODES.includes(stateRaw.cull)) fail(`state.cull must be one of ${CULL_MODES.join(', ')}`);
  if (stateRaw.queue !== undefined && !QUEUES.includes(stateRaw.queue)) fail(`state.queue must be one of ${QUEUES.join(', ')}`);
  if (stateRaw.depthWrite !== undefined && typeof stateRaw.depthWrite !== 'boolean') fail('state.depthWrite must be true or false');
  const state = {};
  for (const key of STATE_KEYS) if (stateRaw[key] !== undefined) state[key] = stateRaw[key];

  // Checked here so a mistake is found at cook time; the engine reads it from the file, so the cooked data has none of it
  const fallbackRaw = raw.fallback ?? {};
  if (fallbackRaw === null || typeof fallbackRaw !== 'object' || Array.isArray(fallbackRaw)) fail('"fallback" must be an object');
  for (const key of Object.keys(fallbackRaw)) {
    if (!FALLBACK_KEYS.includes(key)) fail(`unknown fallback key "${key}" (known: ${FALLBACK_KEYS.join(', ')})`);
  }
  const named = (name, types) => params.some((param) => param.name === name && types.includes(param.type));
  if (fallbackRaw.tint !== undefined && !named(fallbackRaw.tint, ['color', 'vec4'])) fail('fallback.tint must name a color or vec4 parameter');
  if (fallbackRaw.opacity !== undefined && !named(fallbackRaw.opacity, ['float'])) fail('fallback.opacity must name a float parameter');
  if (fallbackRaw.hidden !== undefined && typeof fallbackRaw.hidden !== 'boolean') fail('fallback.hidden must be true or false');

  const targetsRaw = raw.targets ?? {};
  if (targetsRaw === null || typeof targetsRaw !== 'object' || Array.isArray(targetsRaw)) fail('"targets" must be an object');
  let webgpu = 'generated';
  for (const [target, mode] of Object.entries(targetsRaw)) {
    if (target !== 'webgpu') fail(`unknown target "${target}" in "targets"`);
    if (mode !== 'generated' && mode !== 'authored') fail('"targets": {"webgpu"} must be "generated" or "authored"');
    webgpu = mode;
  }

  return { name: raw.name, params, uses, state, targets: { webgpu } };
}

/**
 * Parses a .n2shader file.
 * @param {string} text the file's contents
 * @param {{ path?: string }} [options] the path cited in diagnostics
 * @returns {{ header: object, source: string, firstLine: number, lines: number }}
 *   `source` is the fragment stage text, `firstLine` its first line's number in the file
 */
export function parseN2Shader(text, { path = 'shader.n2shader' } = {}) {
  const fail = (message, line = null, rule = null) => { throw new ShaderError('parse', message, { file: path, line, rule }); };
  if (typeof text !== 'string') fail('the file is not text');
  if (text.includes('\0')) fail('the file contains a NUL byte');
  if (Buffer.byteLength(text, 'utf8') > LIMITS.sourceBytes) fail(`the file is over ${LIMITS.sourceBytes / 1024} KiB`);
  const stripped = text.charCodeAt(0) === 0xFEFF ? text.slice(1) : text;
  const lines = stripped.split('\n').map((line) => line.replace(/\r$/, ''));
  if (lines.length > LIMITS.sourceLines) fail(`the file is over ${LIMITS.sourceLines} lines`);

  const stageIndex = lines.findIndex((line) => line === '#stage fragment');
  if (stageIndex < 0) fail('no "#stage fragment" line (the header must be followed by a line that is exactly #stage fragment)');
  let wgslIndex = -1; // the optional "#stage wgsl" (authored WebGPU), after the fragment stage
  let vertexIndex = -1; // the optional "#stage vertex" (a custom vertex stage, stage 9d), after the fragment stage
  for (let index = 0; index < lines.length; index += 1) {
    if (index === stageIndex || !/^\s*#\s*stage\b/.test(lines[index])) continue;
    if (index < stageIndex) fail('"#stage" inside the header', index + 1);
    if (lines[index] === '#stage wgsl' && wgslIndex < 0) { wgslIndex = index; continue; }
    if (lines[index] === '#stage vertex' && vertexIndex < 0) { vertexIndex = index; continue; }
    fail(`unknown or repeated stage line ${JSON.stringify(lines[index].trim())}`, index + 1);
  }
  // Each stage runs to the next stage line (or the end of the file)
  const stageEnd = (index) => [wgslIndex, vertexIndex].filter((other) => other > index).sort((a, b) => a - b)[0] ?? lines.length;

  const headerText = lines.slice(0, stageIndex).join('\n');
  if (Buffer.byteLength(headerText, 'utf8') > LIMITS.headerBytes) fail(`the header is over ${LIMITS.headerBytes / 1024} KiB`);
  let raw;
  try {
    raw = JSON.parse(headerText);
  } catch (error) {
    fail(`the header is not valid JSON: ${error.message}`, 1);
  }
  const header = validateHeader(raw, (message) => fail(message, 1));
  const authored = header.targets.webgpu === 'authored';
  if (authored && wgslIndex < 0) fail('"targets": {"webgpu": "authored"} needs a "#stage wgsl" block after the fragment stage', stageIndex + 1);
  if (!authored && wgslIndex >= 0) fail('a "#stage wgsl" block needs "targets": {"webgpu": "authored"} in the header (otherwise the WGSL is generated)', wgslIndex + 1);
  const source = lines.slice(stageIndex + 1, stageEnd(stageIndex)).join('\n');
  if (source.trim() === '') fail('the fragment stage is empty', stageIndex + 1);
  const parsed = { header, source, firstLine: stageIndex + 2, lines: lines.length };
  if (wgslIndex >= 0) {
    parsed.wgsl = lines.slice(wgslIndex + 1, stageEnd(wgslIndex)).join('\n');
    parsed.wgslFirstLine = wgslIndex + 2;
  }
  if (vertexIndex >= 0) {
    parsed.vertex = lines.slice(vertexIndex + 1, stageEnd(vertexIndex)).join('\n');
    parsed.vertexFirstLine = vertexIndex + 2;
    if (parsed.vertex.trim() === '') fail('the vertex stage is empty', vertexIndex + 1);
  }
  return parsed;
}
