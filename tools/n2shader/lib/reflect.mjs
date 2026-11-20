// The checks on what the compilers made, independent of running them: the SPIR-V reflection self-check (std140 offsets
// glslang assigned == the offsets the cook computed, only contract sets/bindings/locations, one colour output, only
// allowed capabilities) and the WGSL layout check of the WebGPU payload (proposal sections 2.1 layer 3 and 13.13). Each
// takes parsed data and returns diagnostics, so tests feed it fixtures.
import {
  ALLOWED_CAPABILITIES, LIGHTING, LIGHTING_BLOCK_SIZE, OUTPUT_NAME, PER_DRAW, SAMPLER_BINDING_BASE, TEXTURE_BINDING_BASE, VARYINGS, VERTEX_INPUTS,
} from './contract.mjs';
import { alignUp } from './std140.mjs';

const error = (stage, message) => ({ stage, severity: 'error', file: null, line: null, message });

/**
 * @param {object} json the spirv-cross --reflect output, parsed
 * @param {{ target: 'vulkan'|'webgpu', header: object, layout: { members: object[], size: number } }} context
 */
export function checkReflection(json, { target, header, layout }) {
  const out = [];
  const fail = (message) => out.push(error('reflect', `${target}: ${message}`));
  const lighting = header.uses.includes('lighting');

  for (const key of ['ssbos', 'images', 'subpass_inputs', 'push_constants', 'atomic_counters', 'acceleration_structures']) {
    if (Array.isArray(json[key]) && json[key].length > 0) fail(`the module declares ${key.replace('_', ' ')}, which the contract does not offer`);
  }

  const expected = new Map([['0:0', 'PerDraw']]);
  if (lighting) expected.set('0:1', 'Lighting');
  if (layout.members.length > 0) expected.set('1:0', 'Material');
  const found = new Map();
  for (const ubo of json.ubos ?? []) {
    const key = `${ubo.set ?? 0}:${ubo.binding ?? 0}`;
    if (!expected.has(key)) {
      fail(`unexpected uniform block "${ubo.name}" at set ${ubo.set}, binding ${ubo.binding} (the contract has ${[...expected].map(([k, n]) => `${n} at ${k.replace(':', '/')}`).join(', ')})`);
      continue;
    }
    if (ubo.name !== expected.get(key)) fail(`the block at set/binding ${key.replace(':', '/')} is named "${ubo.name}", expected "${expected.get(key)}"`);
    found.set(key, ubo);
  }

  const memberTable = (ubo) => (json.types?.[ubo.type]?.members ?? []);
  const compare = (label, members, wanted, blockSize, wantedSize) => {
    if (members.length !== wanted.length) {
      fail(`${label} has ${members.length} members, the contract has ${wanted.length}`);
      return;
    }
    wanted.forEach((want, index) => {
      const got = members[index];
      if (got.name !== want.name) fail(`${label} member ${index} is "${got.name}", expected "${want.name}"`);
      else if (got.offset !== want.offset) fail(`${label}.${want.name}: glslang put it at offset ${got.offset}, the cook computed ${want.offset}`);
      else if (want.type !== 'struct' && got.type !== (want.glsl ?? want.type)) fail(`${label}.${want.name}: type ${got.type}, expected ${want.glsl ?? want.type}`);
    });
    if (alignUp(blockSize, 16) !== alignUp(wantedSize, 16)) fail(`${label} is ${blockSize} bytes in the module, expected ${wantedSize}`);
  };
  if (found.has('0:0')) {
    const ubo = found.get('0:0');
    compare('PerDraw', memberTable(ubo), PER_DRAW, ubo.block_size, 220);
  } else fail('the PerDraw block (set 0, binding 0) is missing from the module');
  if (lighting) {
    if (found.has('0:1')) {
      const ubo = found.get('0:1');
      compare('Lighting', memberTable(ubo), LIGHTING, ubo.block_size, LIGHTING_BLOCK_SIZE);
    } else fail('the Lighting block (set 0, binding 1) is missing from the module');
  }
  if (layout.members.length > 0) {
    if (found.has('1:0')) {
      const ubo = found.get('1:0');
      compare('Material', memberTable(ubo), layout.members, ubo.block_size, layout.size);
    } else fail('the Material block (set 1, binding 0) is missing from the module');
  }

  const slots = header.params.filter((param) => param.type === 'texture');
  if (target === 'vulkan') {
    if ((json.separate_images ?? []).length + (json.separate_samplers ?? []).length > 0) fail('separate textures and samplers in the Vulkan module (the Vulkan target uses combined samplers)');
    const textures = json.textures ?? [];
    for (const slot of slots) {
      const binding = TEXTURE_BINDING_BASE + slot.slot;
      if (!textures.some((entry) => entry.set === 1 && entry.binding === binding && entry.type === 'sampler2D')) {
        fail(`texture slot ${slot.slot} ("${slot.name}") is not a sampler2D at set 1, binding ${binding}`);
      }
    }
    for (const entry of textures) {
      const index = entry.binding - TEXTURE_BINDING_BASE;
      if (entry.set !== 1 || !slots.some((slot) => slot.slot === index)) fail(`unexpected sampler "${entry.name}" at set ${entry.set}, binding ${entry.binding}`);
    }
  } else {
    if ((json.textures ?? []).length > 0) fail(`a combined sampler2D (${json.textures.map((entry) => entry.name).join(', ')}) in the WebGPU module: Tint would invent bindings for it and naga rejects it`);
    const images = json.separate_images ?? [];
    const samplers = json.separate_samplers ?? [];
    for (const slot of slots) {
      if (!images.some((entry) => entry.set === 1 && entry.binding === TEXTURE_BINDING_BASE + slot.slot)) fail(`texture slot ${slot.slot} is not a texture2D at set 1, binding ${TEXTURE_BINDING_BASE + slot.slot}`);
      if (!samplers.some((entry) => entry.set === 1 && entry.binding === SAMPLER_BINDING_BASE + slot.slot)) fail(`texture slot ${slot.slot} has no sampler at set 1, binding ${SAMPLER_BINDING_BASE + slot.slot}`);
    }
    for (const entry of images) {
      if (entry.set !== 1 || !slots.some((slot) => TEXTURE_BINDING_BASE + slot.slot === entry.binding)) fail(`unexpected texture "${entry.name}" at set ${entry.set}, binding ${entry.binding}`);
    }
    for (const entry of samplers) {
      if (entry.set !== 1 || !slots.some((slot) => SAMPLER_BINDING_BASE + slot.slot === entry.binding)) fail(`unexpected sampler "${entry.name}" at set ${entry.set}, binding ${entry.binding}`);
    }
  }

  for (const input of json.inputs ?? []) {
    if (!VARYINGS.some((varying) => varying.name === input.name && varying.location === input.location)) {
      fail(`unexpected input "${input.name}" at location ${input.location} (the contract: ${VARYINGS.map((v) => `${v.name}@${v.location}`).join(', ')})`);
    }
  }
  const outputs = json.outputs ?? [];
  if (outputs.length !== 1 || outputs[0].location !== 0 || outputs[0].type !== 'vec4' || outputs[0].name !== OUTPUT_NAME) {
    fail(`the module must have exactly one vec4 output "${OUTPUT_NAME}" at location 0 (found ${outputs.map((entry) => `${entry.type} ${entry.name}@${entry.location}`).join(', ') || 'none'})`);
  }
  return out;
}

/**
 * The self-check of a custom vertex stage's SPIR-V (stage 9d): the PerDraw and Material blocks at the contract's offsets, nothing else
 * bound (no lighting, no textures), the engine's attributes in and the five varyings out.
 */
export function checkVertexReflection(json, { target, header, layout }) {
  const out = [];
  const fail = (message) => out.push(error('reflect', `${target} vertex: ${message}`));
  for (const key of ['ssbos', 'images', 'subpass_inputs', 'push_constants', 'atomic_counters', 'acceleration_structures', 'textures', 'separate_images', 'separate_samplers']) {
    if (Array.isArray(json[key]) && json[key].length > 0) fail(`the vertex stage declares ${key.replace('_', ' ')}, which v1 does not offer there`);
  }
  const expected = new Map([['0:0', 'PerDraw']]);
  if (layout.members.length > 0) expected.set('1:0', 'Material');
  const found = new Map();
  for (const ubo of json.ubos ?? []) {
    const key = `${ubo.set ?? 0}:${ubo.binding ?? 0}`;
    if (!expected.has(key)) { fail(`unexpected uniform block "${ubo.name}" at set ${ubo.set}, binding ${ubo.binding}`); continue; }
    if (ubo.name !== expected.get(key)) fail(`the block at set/binding ${key.replace(':', '/')} is named "${ubo.name}", expected "${expected.get(key)}"`);
    found.set(key, ubo);
  }
  const members = (ubo) => (json.types?.[ubo.type]?.members ?? []);
  const compare = (label, got, wanted) => {
    if (got.length !== wanted.length) { fail(`${label} has ${got.length} members, the contract has ${wanted.length}`); return; }
    wanted.forEach((want, index) => {
      if (got[index].name !== want.name) fail(`${label} member ${index} is "${got[index].name}", expected "${want.name}"`);
      else if (got[index].offset !== want.offset) fail(`${label}.${want.name}: glslang put it at offset ${got[index].offset}, the cook computed ${want.offset}`);
    });
  };
  if (found.has('0:0')) compare('PerDraw', members(found.get('0:0')), PER_DRAW);
  else fail('the PerDraw block (set 0, binding 0) is missing from the module');
  if (layout.members.length > 0) {
    if (found.has('1:0')) compare('Material', members(found.get('1:0')), layout.members);
    else fail('the Material block (set 1, binding 0) is missing from the module');
  }
  for (const input of json.inputs ?? []) {
    if (!VERTEX_INPUTS.some((entry) => entry.name === input.name && entry.location === input.location)) {
      fail(`unexpected input "${input.name}" at location ${input.location} (the contract: ${VERTEX_INPUTS.map((v) => `${v.name}@${v.location}`).join(', ')})`);
    }
  }
  for (const output of json.outputs ?? []) {
    if (!VARYINGS.some((entry) => entry.name === output.name && entry.location === output.location)) {
      fail(`unexpected output "${output.name}" at location ${output.location} (the contract: ${VARYINGS.map((v) => `${v.name}@${v.location}`).join(', ')})`);
    }
  }
  if ((json.outputs ?? []).length !== VARYINGS.length) fail(`the vertex stage must write all five varyings (found ${(json.outputs ?? []).length})`);
  return out;
}

/** The capabilities and extensions a disassembled module (spirv-dis output) declares must be on the allow-list */
export function checkCapabilities(disassembly, target) {
  const out = [];
  for (const match of disassembly.matchAll(/^\s*OpCapability\s+(\w+)/gm)) {
    if (!ALLOWED_CAPABILITIES.includes(match[1])) out.push(error('spirv-dis', `${target}: the module declares capability ${match[1]}, which is not allowed (allowed: ${ALLOWED_CAPABILITIES.join(', ')})`));
  }
  return out;
}

// WGSL size and alignment of the scalar and vector types a Material member can have
function wgslTypeInfo(type) {
  const trimmed = type.trim();
  if (/^(?:f32|i32|u32)$/.test(trimmed)) return { size: 4, align: 4 };
  // a row_major mat3 or mat4 in the SPIR-V is the transposed WGSL matrix of the same size: 48 bytes and 64, 16-aligned
  if (/^mat3x3(?:<\s*f32\s*>|f)$/.test(trimmed)) return { size: 48, align: 16 };
  if (/^mat4x4(?:<\s*f32\s*>|f)$/.test(trimmed)) return { size: 64, align: 16 };
  const vector = /^vec([234])<\s*(?:f32|i32|u32)\s*>$/.exec(trimmed) ?? /^vec([234])(?:f|i|u)$/.exec(trimmed);
  if (vector) {
    const count = Number(vector[1]);
    return { size: 4 * count, align: count === 2 ? 8 : 16 };
  }
  return null;
}

/**
 * The WGSL the WebGPU target produced: the Material struct's members must sit at the offsets the cook computed (equality
 * with glslang's std140 does not imply equality with the WGSL layout), and the bindings must be the contract's.
 */
export function checkWgslLayout(wgsl, { header, layout }) {
  const out = [];
  const fail = (message) => out.push(error('wgsl-layout', message));
  if (layout.members.length > 0) {
    let material = null;
    for (const match of wgsl.matchAll(/struct\s+(\w+)\s*\{([^}]*)\}/g)) {
      const members = [...match[2].matchAll(/(?:@\w+(?:\([^)]*\))?\s*)*(\w+)\s*:\s*([^,]+?)\s*(?:,|$)/g)].map((entry) => ({ name: entry[1], type: entry[2] }));
      if (members.length > 0 && members[0].name === layout.members[0].name) material = { name: match[1], members };
    }
    if (!material) fail(`the WGSL has no struct starting with the Material member "${layout.members[0].name}"`);
    else {
      let cursor = 0;
      let structAlign = 4;
      material.members.forEach((member, index) => {
        const info = wgslTypeInfo(member.type);
        if (!info) { fail(`Material.${member.name}: unsupported WGSL type ${member.type}`); return; }
        const offset = alignUp(cursor, info.align);
        cursor = offset + info.size;
        structAlign = Math.max(structAlign, info.align);
        if (layout.members[index]?.name !== member.name) fail(`Material member ${index} is "${member.name}" in the WGSL, expected "${layout.members[index]?.name}"`);
        else if (layout.members[index].offset !== offset) fail(`Material.${member.name}: the WGSL layout puts it at offset ${offset}, the cook computed ${layout.members[index].offset}`);
      });
      if (material.members.length !== layout.members.length) fail(`the WGSL Material struct has ${material.members.length} members, expected ${layout.members.length}`);
      const size = alignUp(cursor, structAlign);
      if (alignUp(size, 16) !== layout.size) fail(`the WGSL Material struct is ${size} bytes (16-aligned ${alignUp(size, 16)}), the block is ${layout.size}`);
      const binding = new RegExp(`@group\\((\\d+)u?\\)\\s*@binding\\((\\d+)u?\\)\\s*var<uniform>\\s+\\w+\\s*:\\s*${material.name}\\b`).exec(wgsl);
      if (!binding || Number(binding[1]) !== 1 || Number(binding[2]) !== 0) fail(`the Material uniform must be at @group(1) @binding(0) (found ${binding ? `${binding[1]}/${binding[2]}` : 'none'})`);
    }
  }
  for (const param of header.params.filter((entry) => entry.type === 'texture')) {
    const texture = new RegExp(`@group\\((\\d+)u?\\)\\s*@binding\\((\\d+)u?\\)\\s*var\\s+n2_tex${param.slot}\\s*:\\s*texture_2d<f32>`).exec(wgsl);
    if (!texture || Number(texture[1]) !== 1 || Number(texture[2]) !== TEXTURE_BINDING_BASE + param.slot) fail(`texture slot ${param.slot} must be @group(1) @binding(${TEXTURE_BINDING_BASE + param.slot}) (found ${texture ? `${texture[1]}/${texture[2]}` : 'none'})`);
    const sampler = new RegExp(`@group\\((\\d+)u?\\)\\s*@binding\\((\\d+)u?\\)\\s*var\\s+n2_samp${param.slot}\\s*:\\s*sampler\\b`).exec(wgsl);
    if (!sampler || Number(sampler[1]) !== 1 || Number(sampler[2]) !== SAMPLER_BINDING_BASE + param.slot) fail(`sampler slot ${param.slot} must be @group(1) @binding(${SAMPLER_BINDING_BASE + param.slot}) (found ${sampler ? `${sampler[1]}/${sampler[2]}` : 'none'})`);
  }
  return out;
}

// The members the OpenGL prelude spells differently from the SPIR-V blocks (a GLSL function n2_EncodeOutput exists, so the flag is
// n2_EncodeOutputFlag), and the uniform the OpenGL prelude adds (the target's height, for n2_FragCoord())
const GL_RENAMES = Object.freeze({ n2_EncodeOutput: 'n2_EncodeOutputFlag' });
const GL_EXTRA_UNIFORMS = Object.freeze({ n2_TargetHeight: 'float' });

/**
 * The OpenGL payload (GLSL 330 text) against the SPIR-V reflection of the same shader (proposal section 13.11): GL cannot run in CI,
 * and the renderer sets every uniform BY NAME, so the names and types of the plain uniforms the text declares must be exactly the
 * members glslang reflected from the PerDraw, Lighting (when used) and Material blocks, plus the one OpenGL extra, plus one
 * sampler2D per texture slot. Anything else, or anything missing, is an error: a drifted name would be silently ignored by GL
 * (location -1) and the shader would draw with zeros.
 * @param {string} glsl the generated GL text
 * @param {object} json the spirv-cross --reflect output of the Vulkan target of the same shader
 */
export function checkGlUniforms(glsl, json, { header, layout }) {
  const out = [];
  const fail = (message) => out.push(error('gl-uniforms', `gl: ${message}`));
  const lighting = header.uses.includes('lighting');

  // the plain uniforms the GL text declares (top level, `uniform <type> <name>[N];`); a struct-typed array is one uniform here
  const declared = new Map();
  for (const match of glsl.matchAll(/^\s*uniform\s+(\w+)\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*;/gm)) declared.set(match[2], match[1]);

  const blocks = new Map();
  for (const ubo of json.ubos ?? []) blocks.set(ubo.name, json.types?.[ubo.type]?.members ?? []);
  const expected = new Map();
  const addBlock = (name, wantBlock) => {
    const members = blocks.get(name);
    if (!members) { if (wantBlock) fail(`the SPIR-V reflection has no ${name} block to compare with`); return; }
    for (const member of members) {
      const glName = GL_RENAMES[member.name] ?? member.name;
      // a struct member (a light array) has the struct's name as its type: the GL text names the struct type itself
      expected.set(glName, member.type);
    }
  };
  addBlock('PerDraw', true);
  if (lighting) addBlock('Lighting', true);
  if (layout.members.length > 0) addBlock('Material', true);
  for (const [name, type] of Object.entries(GL_EXTRA_UNIFORMS)) expected.set(name, type);
  for (const param of header.params.filter((entry) => entry.type === 'texture')) expected.set(`n2_tex${param.slot}`, 'sampler2D');

  for (const [name, type] of expected) {
    if (!declared.has(name)) { fail(`the uniform "${name}" (${type}) the renderer sets by name is not declared in the GL text`); continue; }
    const glType = declared.get(name);
    const isStruct = /^(?:struct|N2\w+)$/.test(glType) || /^N2\w+$/.test(glType);
    if (!isStruct && glType !== type && !(type === 'bool' && glType === 'int')) fail(`the uniform "${name}" is ${glType} in the GL text and ${type} in the SPIR-V reflection`);
  }
  for (const [name, type] of declared) {
    if (!expected.has(name)) fail(`the GL text declares the uniform "${name}" (${type}), which the SPIR-V reflection of the same shader has no counterpart for`);
  }
  return out;
}
