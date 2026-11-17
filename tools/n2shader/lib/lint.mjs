// The user-stage lint (proposal sections 2.1, 5.2, 8 and 13.2): mechanical checks that run before any compiler, each with
// a message that says what to write instead. Line numbers are the .n2shader file's.
import { ALLOWED_EXTENSIONS } from './contract.mjs';

/** Replaces comments and string literals by spaces (newlines kept), so the rules never match inside them */
export function stripComments(source) {
  let out = '';
  let index = 0;
  while (index < source.length) {
    const char = source[index];
    const next = source[index + 1];
    if (char === '/' && next === '/') {
      while (index < source.length && source[index] !== '\n') { out += ' '; index += 1; }
    } else if (char === '/' && next === '*') {
      out += '  ';
      index += 2;
      while (index < source.length && !(source[index] === '*' && source[index + 1] === '/')) {
        out += source[index] === '\n' ? '\n' : ' ';
        index += 1;
      }
      if (index < source.length) { out += '  '; index += 2; }
    } else if (char === '"') {
      out += ' ';
      index += 1;
      while (index < source.length && source[index] !== '"' && source[index] !== '\n') { out += ' '; index += 1; }
      if (source[index] === '"') { out += ' '; index += 1; }
    } else {
      out += char;
      index += 1;
    }
  }
  return out;
}

// builtins and names the contract does not offer: [pattern, message]
const BANNED = [
  [/\bisnan\b/, 'isnan() is not supported: WGSL cannot represent NaN, so Tint and naga both reject it'],
  [/\bisinf\b/, 'isinf() is not supported: WGSL cannot represent infinities, so Tint and naga both reject it; compare against a large finite value instead'],
  [/\bgl_Position\b/, 'gl_Position is not available: v1 supplies the vertex stage (a custom vertex stage comes later with n2_SetPosition)'],
  [/\bgl_(?:VertexID|InstanceID|VertexIndex|InstanceIndex)\b/, 'gl_VertexID and gl_InstanceID are not available in v1 (the fragment stage has no such input)'],
  [/\bgl_FragCoord\b/, 'raw gl_FragCoord differs between OpenGL (bottom-left origin) and Vulkan/WebGPU (top-left): use n2_FragCoord(), which counts rows from the top on every target'],
  [/\bgl_FrontFacing\b/, 'gl_FrontFacing flips with the viewport on Vulkan and WebGPU and is not part of the contract'],
  [/\bgl_FragDepth\b/, 'writing gl_FragDepth is not available: the contract has one colour output'],
  [/\bgl_PointCoord\b|\bgl_SampleID\b|\bgl_SamplePosition\b|\bgl_Layer\b|\bgl_ViewportIndex\b|\bgl_PrimitiveID\b/, 'this gl_ built-in variable is not part of the contract'],
  [/\b(?:sampler1D|sampler3D|samplerCube|sampler2DArray|sampler2DShadow|samplerCubeShadow|sampler2DMS|isampler\w+|usampler\w+|image\w+|texture3D|textureCube|texture2DArray)\b/, '3D, cube, array, shadow and integer textures and images are not available in v1 (five 2D textures through n2_texture)'],
  [/\b(?:atomic\w*|barrier|memoryBarrier\w*|groupMemoryBarrier|imageStore|imageLoad|EmitVertex|EndPrimitive)\b/, 'atomics, barriers, image access and geometry built-ins are not available in a fragment-only user shader'],
  [/\b(?:textureGrad|textureProjGrad|textureGather\w*)\b/, 'textureGrad and textureGather are not in the contract; use n2_texture, n2_textureLod or n2_texelFetch'],
];

const DECLARATION_QUALIFIERS = new Set([
  'uniform', 'buffer', 'in', 'out', 'inout', 'layout', 'attribute', 'varying', 'shared', 'patch', 'flat', 'smooth',
  'centroid', 'noperspective', 'coherent', 'volatile', 'restrict', 'readonly', 'writeonly', 'subroutine', 'sample',
]);

/** Matching close of the paren that opens at `open` in `text`, or -1 */
function matchParen(text, open) {
  let depth = 0;
  for (let index = open; index < text.length; index += 1) {
    if (text[index] === '(') depth += 1;
    else if (text[index] === ')') {
      depth -= 1;
      if (depth === 0) return index;
    }
  }
  return -1;
}

function lineOf(text, offset, firstLine) {
  let line = firstLine;
  for (let index = 0; index < offset; index += 1) if (text[index] === '\n') line += 1;
  return line;
}

/** Top-level statements: a statement whose words before the first ( { = [ ; carry an interface, resource or storage qualifier is not allowed */
function topLevelQualifiers(text, firstLine) {
  const found = [];
  let braceDepth = 0;
  let parenDepth = 0;
  let atStart = true;
  let line = firstLine;
  let index = 0;
  while (index < text.length) {
    const char = text[index];
    if (char === '\n') { line += 1; index += 1; continue; }
    if (char === '#') { // a directive line is not a statement
      while (index < text.length && text[index] !== '\n') index += 1;
      continue;
    }
    if (/\s/.test(char)) { index += 1; continue; }
    if (atStart && braceDepth === 0 && parenDepth === 0) {
      atStart = false;
      // the words up to the first ( { = [ ; of this statement
      let scan = index;
      let scanLine = line;
      while (scan < text.length && !"(){}=[;".includes(text[scan])) {
        if (/[A-Za-z_]/.test(text[scan])) {
          let end = scan;
          while (end < text.length && /[A-Za-z0-9_]/.test(text[end])) end += 1;
          const word = text.slice(scan, end);
          if (DECLARATION_QUALIFIERS.has(word)) found.push({ word, line: scanLine });
          scan = end;
        } else {
          if (text[scan] === '\n') scanLine += 1;
          scan += 1;
        }
      }
    }
    if (char === "{") braceDepth += 1;
    else if (char === "}") {
      braceDepth -= 1;
      if (braceDepth === 0) atStart = true;
    } else if (char === "(") parenDepth += 1;
    else if (char === ")") parenDepth -= 1;
    else if (char === ";" && braceDepth === 0 && parenDepth === 0) atStart = true;
    index += 1;
  }
  return found;
}

/**
 * @param {string} source the fragment stage text
 * @param {{ firstLine: number, header: { params: object[] }, strict?: boolean }} options
 * @returns {{ severity: 'error'|'warning', line: number, rule: string, message: string }[]}
 */
export function lintStage(source, { firstLine, header, strict = true }) {
  const diagnostics = [];
  const add = (line, rule, message, severity = 'error') => diagnostics.push({ severity, line, rule, message });
  const text = stripComments(source);
  const lines = text.split('\n');

  lines.forEach((line, index) => {
    const number = firstLine + index;
    for (const [pattern, message] of BANNED) {
      if (pattern.test(line)) add(number, 'banned-builtin', message);
    }
    const directive = /^\s*#\s*(\w+)\s*(.*)$/.exec(line);
    if (directive) {
      const [, name, rest] = directive;
      if (name === 'version') add(number, 'version', '#version is added by the prelude; remove it');
      else if (name === 'extension') {
        const extension = /^([A-Za-z0-9_]+)/.exec(rest)?.[1] ?? '';
        if (!ALLOWED_EXTENSIONS.includes(extension)) {
          add(number, 'extension', `#extension ${extension || '(none)'} is not allowed (allowed: ${ALLOWED_EXTENSIONS.join(', ')})`);
        }
      } else if (name === 'line') add(number, 'line', '#line would break the file:line mapping of errors; remove it');
      else if (name === 'pragma' || name === 'include') add(number, name, `#${name} is not allowed in a user stage`);
    }
    if (strict && /\bwhile\b/.test(line)) {
      add(number, 'loop', 'while loops are not allowed in strict mode (a loop with no static bound can hang the GPU); use a for loop with a constant bound, or cook with --relaxed');
    }
    if (strict && /\bdo\b(?=\s*[{\w])/.test(line)) {
      add(number, 'loop', 'do loops are not allowed in strict mode; use a for loop with a constant bound, or cook with --relaxed');
    }
  });

  for (const { word, line } of topLevelQualifiers(text, firstLine)) {
    add(line, 'declaration', `a global "${word}" declaration is not allowed: resources, inputs and outputs come from the contract (the prelude and the header's params); write constants as "const"`);
  }

  if (strict) {
    const constants = new Set([...text.matchAll(/\bconst\s+(?:highp\s+|mediump\s+|lowp\s+)?(?:int|uint)\s+([A-Za-z_]\w*)\s*=/g)].map((match) => match[1]));
    const boundedParams = new Set(header.params.filter((param) => param.type === 'int' && Number.isFinite(param.max)).map((param) => param.name));
    const textureSlots = new Set(header.params.filter((param) => param.type === 'texture').map((param) => param.name));
    for (const match of text.matchAll(/\bfor\s*\(/g)) {
      const open = match.index + match[0].length - 1;
      const close = matchParen(text, open);
      const line = lineOf(text, match.index, firstLine);
      if (close < 0) continue;
      const clauses = text.slice(open + 1, close).split(';');
      if (clauses.length !== 3) { add(line, 'loop', 'a for loop must have the three clauses (init; condition; step)'); continue; }
      const condition = /^\s*([A-Za-z_]\w*)\s*(<=|<|>=|>|!=)\s*([\s\S]+?)\s*$/.exec(clauses[1]);
      const bound = condition?.[3];
      const literal = bound !== undefined && /^\d+$/.test(bound);
      const named = bound !== undefined && /^[A-Za-z_]\w*$/.test(bound)
        && (constants.has(bound) || boundedParams.has(bound) || textureSlots.has(bound));
      if (!condition || !(literal || named)) {
        add(line, 'loop', 'a for loop needs a static bound: compare the counter against an integer literal, a "const int", or an int parameter with a declared "max" (or cook with --relaxed)');
      }
    }
  }

  const mains = [...text.matchAll(/\bvoid\s+main\s*\(\s*(?:void)?\s*\)/g)];
  if (mains.length === 0) add(firstLine, 'main', 'the stage has no "void main()"');
  if (mains.length > 1) add(lineOf(text, mains[1].index, firstLine), 'main', 'the stage has more than one main()');

  diagnostics.sort((a, b) => a.line - b.line || a.rule.localeCompare(b.rule));
  return diagnostics;
}
