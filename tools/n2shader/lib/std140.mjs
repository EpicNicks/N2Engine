// std140 layout of the generated Material block. Declaration order is kept (no reordering): the author, the manifest and
// the runtime all see the same member sequence. Rules used (GLSL 4.50 section 7.6.2.2): a scalar aligns to 4, a vec2 to 8,
// a vec3 and a vec4 to 16; a vec3 takes 12 bytes, so a float or int may follow it in the same 16 bytes; the block's size is
// rounded up to 16.

const TYPES = Object.freeze({
  float: { size: 4, align: 4, glsl: 'float' },
  int: { size: 4, align: 4, glsl: 'int' },
  bool: { size: 4, align: 4, glsl: 'int' }, // a bool is not allowed in a uniform block: an int, 0 or 1
  vec2: { size: 8, align: 8, glsl: 'vec2' },
  vec3: { size: 12, align: 16, glsl: 'vec3' },
  vec4: { size: 16, align: 16, glsl: 'vec4' },
  color: { size: 16, align: 16, glsl: 'vec4' },
  // Matrices are row_major in the block (the engine's matrices are row-major, as in the per-draw block): a mat3 is three rows
  // of a vec4's stride (48 bytes), a mat4 four rows (64). `qualifier` is what the SPIR-V targets declare it with
  mat3: { size: 48, align: 16, glsl: 'mat3', qualifier: 'layout(row_major) ' },
  mat4: { size: 64, align: 16, glsl: 'mat4', qualifier: 'layout(row_major) ' },
});

export const alignUp = (value, alignment) => Math.ceil(value / alignment) * alignment;

export function isBlockType(type) {
  return Object.prototype.hasOwnProperty.call(TYPES, type);
}

/** The GLSL spelling of a parameter type (undefined for texture) */
export function glslType(type) {
  return TYPES[type]?.glsl;
}

/**
 * Lays out the non-texture parameters: [{name, type, glsl, offset, size}] and the block size.
 * @param {{name: string, type: string}[]} params
 */
export function layoutBlock(params) {
  const members = [];
  let cursor = 0;
  for (const param of params) {
    const info = TYPES[param.type];
    if (!info) continue; // a texture is not in the block
    const offset = alignUp(cursor, info.align);
    members.push({ name: param.name, type: param.type, glsl: info.glsl, offset, size: info.size, qualifier: info.qualifier ?? '' });
    cursor = offset + info.size;
  }
  return { members, size: members.length === 0 ? 0 : alignUp(cursor, 16) };
}
