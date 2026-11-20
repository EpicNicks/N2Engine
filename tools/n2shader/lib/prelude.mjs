// Generates the per-target GLSL the compilers see: version header, the target's prelude template (renderer/shaders/user/
// prelude/), the generated Material block and texture-slot constants, then the author's stage behind a #line directive
// (proposal sections 3.4 and 13.12). Pure text work, no external tool.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CONTRACT_VERSION, SAMPLER_BINDING_BASE, TEXTURE_BINDING_BASE } from './contract.mjs';
import { layoutBlock } from './std140.mjs';

export const PRELUDE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../renderer/shaders/user/prelude');
const TEMPLATE_FILES = { vulkan: 'vulkan.glsl', webgpu: 'webgpu.glsl', gl: 'opengl.glsl' };

/** A template file's text with LF endings (a Windows checkout has CRLF; the output must not depend on it) */
function readTemplate(name, dir) {
  return fs.readFileSync(path.join(dir, name), 'utf8').replace(/\r\n/g, '\n');
}

/** Expands //@include, //@if-lighting and //@each-texture; `slots` is the list of texture slot numbers */
export function expandTemplate(text, { lighting, slots, dir = PRELUDE_DIR, depth = 0 }) {
  if (depth > 4) throw new Error('prelude includes nest too deeply');
  const out = [];
  const lines = text.split('\n');
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    const trimmed = line.trim();
    const include = /^\/\/@include\s+(\S+)$/.exec(trimmed);
    if (include) {
      out.push(...expandTemplate(readTemplate(include[1], dir), { lighting, slots, dir, depth: depth + 1 }).split('\n').slice(0, -1));
      continue;
    }
    const block = trimmed === '//@if-lighting' ? 'if' : trimmed === '//@each-texture' ? 'each' : null;
    if (!block) {
      out.push(line);
      continue;
    }
    const closer = block === 'if' ? '//@end-if' : '//@end-each';
    let end = index + 1;
    while (end < lines.length && lines[end].trim() !== closer) end += 1;
    if (end >= lines.length) throw new Error(`prelude template: ${trimmed} without ${closer}`);
    const body = lines.slice(index + 1, end).join('\n');
    if (block === 'if') {
      if (lighting) out.push(...expandTemplate(`${body}\n`, { lighting, slots, dir, depth }).split('\n').slice(0, -1));
    } else {
      for (const slot of slots) {
        const filled = body
          .replaceAll('@SLOT@', String(slot))
          .replaceAll('@TEX_BINDING@', String(TEXTURE_BINDING_BASE + slot))
          .replaceAll('@SAMP_BINDING@', String(SAMPLER_BINDING_BASE + slot));
        out.push(...filled.split('\n'));
      }
    }
    index = end;
  }
  return `${out.join('\n')}\n`;
}

/** The generated Material parameters: a std140 block (SPIR-V targets) or plain uniforms (OpenGL), plus texture slot constants */
export function materialSection(target, header) {
  const { members } = layoutBlock(header.params);
  const lines = [`// Generated from the header's params (contract ${CONTRACT_VERSION}); a bool is an int (0 or 1), a texture is its slot number for n2_texture`];
  if (members.length > 0) {
    if (target === 'gl') {
      for (const member of members) lines.push(`uniform ${member.glsl} ${member.name};`);
    } else {
      lines.push('layout(set = 1, binding = 0, std140) uniform Material {');
      for (const member of members) lines.push(`    ${member.qualifier}${member.glsl} ${member.name};`);
      lines.push('};');
    }
  }
  for (const param of header.params) {
    if (param.type === 'texture') lines.push(`const int ${param.name} = ${param.slot};`);
  }
  return `${lines.join('\n')}\n`;
}

/** Whether the author's stage names isnan or isinf: then the NaN shims (nan-shims.glsl) follow the prelude. Plain word search, the C++ twin's rule */
export function usesNanChecks(source) {
  return /\b(?:isnan|isinf)\b/.test(source);
}

const linePath = (file) => file.replace(/\\/g, '/').replace(/["\r\n]/g, '_');

/**
 * The complete GLSL of one target.
 * @param {'vulkan'|'webgpu'|'gl'} target
 * @param {{ header: object, source: string, firstLine: number }} parsed the parseN2Shader result
 * @param {{ path?: string, dir?: string }} [options] `path` is what #line cites (the file's path as the author knows it)
 * @returns {{ text: string, userLine: number }} `userLine` is the generated line number of the author's first line
 */
export function buildTargetSource(target, parsed, { path: file = 'shader.n2shader', dir = PRELUDE_DIR } = {}) {
  if (!Object.hasOwn(TEMPLATE_FILES, target)) throw new Error(`unknown target "${target}"`);
  const { header } = parsed;
  const slots = header.params.filter((param) => param.type === 'texture').map((param) => param.slot).sort((a, b) => a - b);
  const prelude = expandTemplate(readTemplate(TEMPLATE_FILES[target], dir), { lighting: header.uses.includes('lighting'), slots, dir });
  const nanShims = usesNanChecks(parsed.source) ? readTemplate('nan-shims.glsl', dir) : '';
  const head = target === 'gl'
    ? '#version 330 core\n'
    : '#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n#extension GL_EXT_samplerless_texture_functions : enable\n';
  const lineDirective = target === 'gl' ? `#line ${parsed.firstLine} 1\n` : `#line ${parsed.firstLine} "${linePath(file)}"\n`;
  // With a custom vertex stage the engine's vertex stage is the author's, which writes all five varyings: the SPIR-V targets' fragment
  // stage is made to read them all, so no vertex output goes unread (the validation layer warns about one). The author's main is renamed
  // for the generated one, which reads the varyings and calls it
  const hasVertex = typeof parsed.vertex === 'string';
  const rename = hasVertex && target !== 'gl' ? '#define main n2_user_main\n' : '';
  const trailer = hasVertex && target !== 'gl'
    ? '#undef main\nvoid main() {\n    float n2_sink = n2_UV.x + n2_Color.x + n2_WorldPos.x + n2_Normal.x + n2_Tangent.x;\n    n2_user_main();\n}\n'
    : '';
  const before = `${head}// n2shader contract ${CONTRACT_VERSION}, target ${target}\n${prelude}${nanShims}${materialSection(target, header)}${rename}${lineDirective}`;
  return { text: `${before}${parsed.source}\n${trailer}`, userLine: before.split('\n').length };
}

const VERTEX_TEMPLATE_FILES = Object.freeze({ vulkan: 'vertex-vulkan.glsl', webgpu: 'vertex-webgpu.glsl', gl: 'vertex-opengl.glsl' });

/**
 * The complete GLSL of one target's custom vertex stage: version header, the vertex prelude (attributes, varyings, PerDraw, the clip
 * remap, n2_DefaultVertex and n2_SetPosition), the Material block (the header's params), the author's n2_vertex() behind a #line, and
 * the generated main. Only for a parsed file that has a vertex stage.
 * @returns {{ text: string, userLine: number }}
 */
export function buildVertexSource(target, parsed, { path: file = 'shader.n2shader', dir = PRELUDE_DIR } = {}) {
  if (typeof parsed.vertex !== 'string') throw new Error('buildVertexSource: the file has no vertex stage');
  const { header } = parsed;
  const prelude = expandTemplate(readTemplate(VERTEX_TEMPLATE_FILES[target], dir), { lighting: false, slots: [], dir });
  const head = target === 'gl' ? '#version 330 core\n' : '#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n';
  const lineDirective = target === 'gl' ? `#line ${parsed.vertexFirstLine} 2\n` : `#line ${parsed.vertexFirstLine} "${linePath(file)}"\n`;
  const nanShims = usesNanChecks(parsed.vertex) ? readTemplate('nan-shims.glsl', dir) : '';
  // The Material block only: a texture is a fragment-stage resource in v1
  const material = materialSection(target, { ...header, params: header.params.filter((param) => param.type !== 'texture') });
  const before = `${head}// n2shader contract ${CONTRACT_VERSION}, target ${target}, vertex stage\n${prelude}${nanShims}${material}${lineDirective}`;
  const trailer = 'void main() {\n    n2_DefaultVertex();\n    n2_vertex();\n    if (!n2_positionSet) {\n        n2_SetPosition(n2_Projection * n2_View * vec4(n2_WorldPos, 1.0));\n    }\n}\n';
  return { text: `${before}${parsed.vertex}\n${trailer}`, userLine: before.split('\n').length };
}
