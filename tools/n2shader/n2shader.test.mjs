// Tests for tools/n2shader. Run: node --test tools/n2shader/n2shader.test.mjs
// The unit tests need no external tool. The cases that run glslang, Tint or spirv-cross are skipped, with the reason in the
// test name, when the tool is not installed; CI installs them (the "wgsl (Tint, naga)" job), so there they run.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { LIGHTING, LIMITS, PER_DRAW, TEXTURE_BINDING_BASE } from './lib/contract.mjs';
import { cookShader, manifestBytes, parseGlslangOutput, writeOutputs } from './lib/cook.mjs';
import { runCorpus } from './lib/corpus.mjs';
import { lintStage, stripComments } from './lib/lint.mjs';
import { ShaderError, parseN2Shader } from './lib/parse.mjs';
import { buildTargetSource, expandTemplate, materialSection, PRELUDE_DIR } from './lib/prelude.mjs';
import { checkCapabilities, checkGlUniforms, checkReflection, checkWgslLayout } from './lib/reflect.mjs';
import { layoutBlock } from './lib/std140.mjs';
import { discoverTools, findTool, TOOL_NAMES } from './lib/tools.mjs';
import { main, parseArguments } from './n2shader.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../..');
const corpusDir = path.join(repo, 'renderer/shaders/user/corpus');
const tmp = (prefix) => fs.mkdtempSync(path.join(os.tmpdir(), prefix));
const noTools = Object.fromEntries(TOOL_NAMES.map((name) => [name, null]));
const realTools = discoverTools();

/** A .n2shader from a header object and a body */
// (a one-line header, so the author's first line is line 3 of the file)
const shader = (header, body = 'void main() { n2_FragColor = vec4(1.0); }') => `${JSON.stringify(header)}\n#stage fragment\n${body}\n`;
const param = (name, type, extra = {}) => ({ name, type, ...extra });

// ---------------------------------------------------------------------------------------------------------------------
// std140: hand-computed cases (GLSL 4.50 7.6.2.2)
// ---------------------------------------------------------------------------------------------------------------------
const std140Cases = [
  ['a lone float', [['float']], [0], 16],
  ['float then float (packed)', [['float'], ['float']], [0, 4], 16],
  ['vec3 then float (the float fills the vec3 tail)', [['vec3'], ['float']], [0, 12], 16],
  ['float then vec3 (the vec3 aligns to 16)', [['float'], ['vec3']], [0, 16], 32],
  ['vec2 after a float aligns to 8', [['float'], ['vec2']], [0, 8], 16],
  ['float, vec2, float', [['float'], ['vec2'], ['float']], [0, 8, 16], 32],
  ['vec4 after a float aligns to 16', [['float'], ['vec4']], [0, 16], 32],
  ['color is a vec4', [['int'], ['color']], [0, 16], 32],
  ['bool is a 4-byte int', [['bool'], ['bool'], ['bool'], ['bool'], ['bool']], [0, 4, 8, 12, 16], 32],
  ['vec3, vec3 (each aligns to 16)', [['vec3'], ['vec3']], [0, 16], 32],
  ['vec3, float, vec3, int', [['vec3'], ['float'], ['vec3'], ['int']], [0, 12, 16, 28], 32],
  ['vec2, vec2, vec2', [['vec2'], ['vec2'], ['vec2']], [0, 8, 16], 32],
  ['the spike shader: color float int vec3 bool', [['color'], ['float'], ['int'], ['vec3'], ['bool']], [0, 16, 20, 32, 44], 48],
  ['textures are not in the block', [['texture'], ['float'], ['texture']], [0], 16],
];
for (const [name, types, offsets, size] of std140Cases) {
  test(`std140: ${name}`, () => {
    const result = layoutBlock(types.map(([type], index) => ({ name: `p${index}`, type })));
    assert.deepEqual(result.members.map((member) => member.offset), offsets);
    assert.equal(result.size, size);
  });
}

test('std140: no members, no block', () => {
  assert.deepEqual(layoutBlock([param('t', 'texture')]), { members: [], size: 0 });
});

// ---------------------------------------------------------------------------------------------------------------------
// Parser and header rejections
// ---------------------------------------------------------------------------------------------------------------------
const header = (extra = {}) => ({ n2shader: 1, name: 'T', ...extra });
const rejects = (text, pattern, options) => assert.throws(() => parseN2Shader(text, options), (error) => {
  assert.ok(error instanceof ShaderError, `a ShaderError, got ${error}`);
  assert.equal(error.stage, 'parse');
  assert.match(error.message, pattern);
  return true;
});

test('parse: a minimal file', () => {
  const parsed = parseN2Shader('{ "n2shader": 1, "name": "A" }\n#stage fragment\nvoid main() {}\n');
  assert.equal(parsed.header.name, 'A');
  assert.deepEqual(parsed.header.params, []);
  assert.deepEqual(parsed.header.uses, []);
  assert.equal(parsed.firstLine, 3);
  assert.equal(parsed.source, 'void main() {}\n');
});

test('parse: CRLF and LF files give the same stage text and line numbers', () => {
  const lf = shader(header({ params: [param('u_A', 'float')] }));
  const crlf = lf.replace(/\n/g, '\r\n');
  const a = parseN2Shader(lf);
  const b = parseN2Shader(crlf);
  assert.deepEqual(a, b);
  assert.ok(!b.source.includes('\r'));
});

test('parse: header defaults per type', () => {
  const { header: h } = parseN2Shader(shader(header({ params: [
    param('a', 'float'), param('b', 'int'), param('c', 'bool'), param('d', 'vec2'), param('e', 'vec3'), param('f', 'vec4'), param('g', 'color'), param('h', 'texture'),
  ] })));
  assert.deepEqual(h.params.map((p) => p.default), [0, 0, false, [0, 0], [0, 0, 0], [0, 0, 0, 0], [1, 1, 1, 1], 'white']);
});

test('parse: baseColorTexture takes slot 0, the others follow in declaration order', () => {
  const { header: h } = parseN2Shader(shader(header({ params: [param('u_A', 'texture'), param('baseColorTexture', 'texture'), param('u_B', 'texture')] })));
  assert.deepEqual(h.params.map((p) => [p.name, p.slot]), [['u_A', 1], ['baseColorTexture', 0], ['u_B', 2]]);
});

const headerRejections = [
  ['no #stage line', '{ "n2shader": 1, "name": "A" }\nvoid main() {}\n', /no "#stage fragment" line/],
  ['#stage vertex', '{ "n2shader": 1, "name": "A" }\n#stage fragment\nvoid main() {}\n#stage vertex\nvoid main() {}\n', /"#stage vertex" is reserved/],
  ['#stage wgsl', '{ "n2shader": 1, "name": "A" }\n#stage fragment\nvoid main() {}\n#stage wgsl\n', /unknown or repeated stage/],
  ['a repeated #stage fragment', '{ "n2shader": 1, "name": "A" }\n#stage fragment\nvoid main() {}\n#stage fragment\n', /unknown or repeated stage/],
  ['invalid JSON', '{ "n2shader": 1, \n#stage fragment\nvoid main() {}\n', /not valid JSON/],
  ['JSON comments (strict JSON)', '{ "n2shader": 1, // note\n "name": "A" }\n#stage fragment\nvoid main() {}\n', /not valid JSON/],
  ['an array header', '[]\n#stage fragment\nvoid main() {}\n', /must be a JSON object/],
  ['no format version', '{ "name": "A" }\n#stage fragment\nvoid main() {}\n', /needs "n2shader"/],
  ['a newer format', '{ "n2shader": 2, "name": "A" }\n#stage fragment\nvoid main() {}\n', /newer than the format 1/],
  ['format 0', '{ "n2shader": 0, "name": "A" }\n#stage fragment\nvoid main() {}\n', /unsupported format version/],
  ['no name', '{ "n2shader": 1 }\n#stage fragment\nvoid main() {}\n', /non-empty "name"/],
  ['an unknown header key', '{ "n2shader": 1, "name": "A", "extra": 1 }\n#stage fragment\nvoid main() {}\n', /unknown header key "extra"/],
  ['an empty stage', '{ "n2shader": 1, "name": "A" }\n#stage fragment\n\n', /stage is empty/],
  ['a NUL byte', '{ "n2shader": 1, "name": "A" }\n#stage fragment\nvoid main() {}\0\n', /NUL/],
];
for (const [name, text, pattern] of headerRejections) test(`parse rejects: ${name}`, () => rejects(text, pattern));

const paramRejections = [
  ['a bad type', [param('u_A', 'mat4')], /type must be one of/],
  ['a duplicate name', [param('u_A', 'float'), param('u_A', 'int')], /declared twice/],
  ['an n2_ name', [param('n2_Model', 'float')], /reserved/],
  ['a gl_ name', [param('gl_X', 'float')], /reserved/],
  ['a keyword name', [param('vec3', 'float')], /keyword/],
  ['a name that is not an identifier', [param('1abc', 'float')], /identifier/],
  ['a double underscore', [param('u__a', 'float')], /double underscore/],
  ['a float default of the wrong type', [param('u_A', 'float', { default: 'x' })], /float default must be a number/],
  ['an int default that is not an integer', [param('u_A', 'int', { default: 1.5 })], /integer/],
  ['a bool default that is a number', [param('u_A', 'bool', { default: 1 })], /true or false/],
  ['a vec3 default of the wrong length', [param('u_A', 'vec3', { default: [1, 2] })], /array of 3 numbers/],
  ['a texture default that is unknown', [param('u_A', 'texture', { default: 'red' })], /one of white, black, normal, none/],
  ['min above max', [param('u_A', 'float', { min: 2, max: 1 })], /min is above max/],
  ['min on a vec3', [param('u_A', 'vec3', { min: 0 })], /float and int only/],
  ['an unknown param key', [param('u_A', 'float', { bogus: 1 })], /unknown key "bogus"/],
  ['baseColor of the wrong type', [param('baseColor', 'float')], /must be a color/],
  ['baseColorTexture of the wrong type', [param('baseColorTexture', 'float')], /must be a texture/],
  ['six textures', Array.from({ length: 6 }, (_, i) => param(`u_T${i}`, 'texture')), /at most 5 textures/],
  ['a block over 256 bytes', Array.from({ length: 17 }, (_, i) => param(`u_V${i}`, 'vec4')), /over the 256-byte limit/],
  ['65 params', Array.from({ length: 65 }, (_, i) => param(`u_P${i}`, 'bool')), /at most 64 parameters/],
];
for (const [name, params, pattern] of paramRejections) test(`header rejects: ${name}`, () => rejects(shader(header({ params })), pattern));

test('header accepts exactly 256 bytes of block and exactly 5 textures', () => {
  const params = [...Array.from({ length: 16 }, (_, i) => param(`u_V${i}`, 'vec4')), ...Array.from({ length: 5 }, (_, i) => param(`u_T${i}`, 'texture'))];
  const { header: h } = parseN2Shader(shader(header({ params })));
  assert.equal(layoutBlock(h.params).size, 256);
});

test('header accepts every blend mode of the contract', () => {
  for (const blend of ['none', 'alpha', 'additive', 'premultiplied', 'multiply']) {
    const { header: h } = parseN2Shader(shader(header({ state: { blend } })));
    assert.equal(h.state.blend, blend);
  }
});

test('header rejects: unknown uses, state values, targets', () => {
  rejects(shader(header({ uses: ['time'] })), /unknown "uses" entry "time"/);
  rejects(shader(header({ uses: 'lighting' })), /"uses" must be an array/);
  rejects(shader(header({ state: { blend: 'screen' } })), /state.blend must be one of/);
  rejects(shader(header({ state: { cull: 'front' } })), /state.cull must be one of/);
  rejects(shader(header({ state: { depthWrite: 'no' } })), /depthWrite must be true or false/);
  rejects(shader(header({ state: { sorting: 1 } })), /unknown state key/);
  rejects(shader(header({ targets: { webgpu: 'authored' } })), /not supported in v1/);
  rejects(shader(header({ targets: { gl: 'generated' } })), /unknown target "gl"/);
});

test('header rejects: nesting beyond the depth limit and oversize inputs', () => {
  let nested = {};
  for (let i = 0; i < LIMITS.headerDepth + 1; i += 1) nested = { a: nested };
  rejects(shader(header({ state: nested })), /nested deeper/);
  rejects(`{ "n2shader": 1, "name": "${'x'.repeat(LIMITS.headerBytes)}" }\n#stage fragment\nvoid main() {}\n`, /header is over/);
  rejects(shader(header(), `// ${'x'.repeat(LIMITS.sourceBytes)}\nvoid main() {}`), /over 256 KiB/);
  rejects(shader(header(), '\n'.repeat(LIMITS.sourceLines + 1)), /over 20000 lines/);
});

// ---------------------------------------------------------------------------------------------------------------------
// Lint
// ---------------------------------------------------------------------------------------------------------------------
const lint = (body, { params = [], strict = true } = {}) => lintStage(body, { firstLine: 10, header: { params }, strict });
const rules = (diagnostics) => diagnostics.map((d) => d.rule);

test('lint: a clean shader has no diagnostics, and lines are the file lines', () => {
  assert.deepEqual(lint('void main() {\n  n2_FragColor = vec4(1.0);\n}\n'), []);
});

for (const [name, body, rule, line] of [
  ['isnan', 'void main() {\n  bool b = isnan(1.0);\n}', 'banned-builtin', 11],
  ['isinf', 'void main() {\n\n  bool b = isinf(1.0);\n}', 'banned-builtin', 12],
  ['gl_Position', 'void main() { gl_Position = vec4(0); }', 'banned-builtin', 10],
  ['gl_VertexID', 'void main() { int i = gl_VertexID; }', 'banned-builtin', 10],
  ['raw gl_FragCoord', 'void main() { vec4 p = gl_FragCoord; }', 'banned-builtin', 10],
  ['gl_FrontFacing', 'void main() { bool f = gl_FrontFacing; }', 'banned-builtin', 10],
  ['a cube sampler', 'void main() { samplerCube c; }', 'banned-builtin', 10],
  ['imageStore', 'void main() { imageStore(a, b, c); }', 'banned-builtin', 10],
  ['#version', '#version 450\nvoid main() {}', 'version', 10],
  ['a non-whitelisted extension', '#extension GL_NV_shader_foo : enable\nvoid main() {}', 'extension', 10],
  ['#line', '#line 5\nvoid main() {}', 'line', 10],
  ['#pragma', '#pragma optimize(off)\nvoid main() {}', 'pragma', 10],
  ['a global uniform', 'uniform float x;\nvoid main() {}', 'declaration', 10],
  ['a global layout block', 'layout(set = 1, binding = 9) uniform Foo { float a; };\nvoid main() {}', 'declaration', 10],
  ['a global in', 'in vec2 extra;\nvoid main() {}', 'declaration', 10],
  ['a global out', 'out vec4 second;\nvoid main() {}', 'declaration', 10],
  ['a global buffer', 'buffer B { float a[]; };\nvoid main() {}', 'declaration', 10],
  ['a while loop', 'void main() {\n  int i = 0;\n  while (i < 3) { i++; }\n}', 'loop', 12],
  ['a do loop', 'void main() {\n  int i = 0;\n  do { i++; } while (i < 3);\n}', 'loop', 12],
  ['no main', 'float f() { return 1.0; }', 'main', 10],
  ['two mains', 'void main() {}\nvoid main() {}', 'main', 11],
]) {
  test(`lint rejects: ${name}`, () => {
    const found = lint(body);
    assert.ok(found.some((d) => d.rule === rule && d.line === line && d.severity === 'error'), `${rule}@${line} in ${JSON.stringify(found)}`);
  });
}

test('lint: the whitelisted extension passes', () => {
  assert.deepEqual(lint('#extension GL_EXT_control_flow_attributes : enable\nvoid main() {}'), []);
});

test('lint: function parameters and local declarations are not global qualifiers', () => {
  assert.deepEqual(lint('float f(in float a, out float b, inout float c) { b = a; c = a; return a; }\nconst float K = 2.0;\nvoid main() { float x = f(1.0, x, x); }'), []);
});

test('lint: comments and strings are ignored', () => {
  assert.deepEqual(lint('// isnan gl_Position while (x)\n/* gl_FragCoord\n isinf */ void main() {}'), []);
  assert.equal(stripComments('a // b\nc /* d\ne */ f').split('\n').length, 3);
});

test('lint: for loops need a static bound in strict mode', () => {
  const ok = (code, params = []) => assert.deepEqual(lint(`void main() { ${code} }`, { params }), [], code);
  const bad = (code, params = []) => assert.ok(rules(lint(`void main() { ${code} }`, { params })).includes('loop'), code);
  ok('for (int i = 0; i < 8; i++) {}');
  ok('for (int i = 8; i > 0; i--) {}');
  ok('for (int i = 0; i <= 4; i += 1) {}');
  bad('for (int i = 0; i < n; i++) {}');
  bad('for (int i = 0; i < u_Steps; i++) {}', [param('u_Steps', 'int')]);
  ok('for (int i = 0; i < u_Steps; i++) {}', [param('u_Steps', 'int', { max: 8 })]);
  ok('const int N = 4; for (int i = 0; i < N; i++) {}');
  bad('for (;;) {}');
  bad('for (float x = 0.0; x < 1.0 && y; x += 0.1) {}');
});

test('lint: relaxed mode allows while, do and unbounded for', () => {
  assert.deepEqual(lint('void main() { int i = 0; while (i < n) { i++; } do { i--; } while (i > 0); for (int j = 0; j < n; j++) {} }', { strict: false }), []);
});

// ---------------------------------------------------------------------------------------------------------------------
// Prelude per target
// ---------------------------------------------------------------------------------------------------------------------
const sampleHeader = header({
  params: [param('u_Tint', 'color'), param('u_Gain', 'float'), param('u_Mask', 'texture'), param('u_Flag', 'bool')],
  uses: ['lighting'],
});
const sampleParsed = () => parseN2Shader(shader(sampleHeader, 'void main() { n2_FragColor = u_Tint; }'));

test('prelude: SPIR-V targets get #version 450 with the line-directive extension, OpenGL gets #version 330 core', () => {
  const parsed = sampleParsed();
  for (const target of ['vulkan', 'webgpu']) {
    const { text } = buildTargetSource(target, parsed, { path: 'res://s.n2shader' });
    assert.match(text, /^#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n/);
    assert.match(text, /#line 3 "res:\/\/s\.n2shader"\nvoid main/);
  }
  const gl = buildTargetSource('gl', parsed, { path: 'res://s.n2shader' }).text;
  assert.match(gl, /^#version 330 core\n/);
  assert.match(gl, /#line 3 1\nvoid main/);
  assert.doesNotMatch(gl, /GL_GOOGLE/);
});

test('prelude: the userLine is the generated line of the first author line', () => {
  for (const target of ['vulkan', 'webgpu', 'gl']) {
    const { text, userLine } = buildTargetSource(target, sampleParsed());
    assert.equal(text.split('\n')[userLine - 1], 'void main() { n2_FragColor = u_Tint; }');
  }
});

test('prelude: the PerDraw block is row_major on Vulkan only, plain uniforms on OpenGL', () => {
  const parsed = sampleParsed();
  const vulkan = buildTargetSource('vulkan', parsed).text;
  const webgpu = buildTargetSource('webgpu', parsed).text;
  const gl = buildTargetSource('gl', parsed).text;
  assert.match(vulkan, /layout\(set = 0, binding = 0, std140, row_major\) uniform PerDraw/);
  assert.match(webgpu, /layout\(set = 0, binding = 0, std140\) uniform PerDraw/);
  assert.doesNotMatch(webgpu, /row_major/);
  assert.match(gl, /^uniform mat4 n2_Model;$/m);
  assert.doesNotMatch(gl, /uniform PerDraw/);
  assert.match(gl, /uniform float n2_TargetHeight;/);
});

test('prelude: textures are combined on Vulkan and OpenGL, separate texture + sampler on WebGPU', () => {
  const parsed = sampleParsed();
  assert.match(buildTargetSource('vulkan', parsed).text, /layout\(set = 1, binding = 1\) uniform sampler2D n2_tex0;/);
  assert.match(buildTargetSource('vulkan', parsed).text, /texture\(n2_tex0, uv\)/);
  const webgpu = buildTargetSource('webgpu', parsed).text;
  assert.match(webgpu, /layout\(set = 1, binding = 1\) uniform texture2D n2_tex0;/);
  assert.match(webgpu, /layout\(set = 1, binding = 6\) uniform sampler n2_samp0;/);
  assert.match(webgpu, /texture\(sampler2D\(n2_tex0, n2_samp0\), uv\)/);
  assert.doesNotMatch(webgpu, /uniform sampler2D/);
  assert.match(buildTargetSource('gl', parsed).text, /^uniform sampler2D n2_tex0;$/m);
});

test('prelude: only the declared texture slots are generated', () => {
  const none = buildTargetSource('vulkan', parseN2Shader(shader(header()))).text;
  assert.doesNotMatch(none, /n2_tex\d/);
  const two = buildTargetSource('webgpu', parseN2Shader(shader(header({ params: [param('baseColorTexture', 'texture'), param('u_B', 'texture')] })))).text;
  assert.match(two, /n2_samp1/);
  assert.doesNotMatch(two, /n2_samp2/);
});

test('prelude: the Material block is generated in declaration order; textures become slot constants', () => {
  const parsed = sampleParsed();
  const vulkan = materialSection('vulkan', parsed.header);
  assert.match(vulkan, /layout\(set = 1, binding = 0, std140\) uniform Material \{\n {4}vec4 u_Tint;\n {4}float u_Gain;\n {4}int u_Flag;\n\};/);
  assert.match(vulkan, /const int u_Mask = 0;/);
  assert.equal(materialSection('webgpu', parsed.header), vulkan);
  const gl = materialSection('gl', parsed.header);
  assert.match(gl, /^uniform vec4 u_Tint;\nuniform float u_Gain;\nuniform int u_Flag;$/m);
  assert.doesNotMatch(gl, /uniform Material/);
  assert.doesNotMatch(materialSection('vulkan', parseN2Shader(shader(header())).header), /uniform Material/);
});

test('prelude: lighting is included only when asked for', () => {
  const lit = sampleParsed();
  const unlit = parseN2Shader(shader(header()));
  for (const target of ['vulkan', 'webgpu', 'gl']) {
    assert.match(buildTargetSource(target, lit).text, /n2_LightDiffuse/);
    assert.doesNotMatch(buildTargetSource(target, unlit).text, /n2_LightDiffuse|Lighting/);
  }
  assert.match(buildTargetSource('vulkan', lit).text, /layout\(set = 0, binding = 1, std140\) uniform Lighting/);
  assert.match(buildTargetSource('gl', lit).text, /^uniform N2PointLight n2_PointLights\[8\];$/m);
});

test('prelude: the output does not depend on the checkout line endings of the templates', () => {
  const copy = tmp('n2shader-prelude-');
  for (const name of fs.readdirSync(PRELUDE_DIR)) {
    fs.writeFileSync(path.join(copy, name), fs.readFileSync(path.join(PRELUDE_DIR, name), 'utf8').replace(/\r\n/g, '\n').replace(/\n/g, '\r\n'));
  }
  for (const target of ['vulkan', 'webgpu', 'gl']) {
    assert.equal(buildTargetSource(target, sampleParsed(), { dir: copy }).text, buildTargetSource(target, sampleParsed()).text);
  }
});

test('prelude: an unterminated directive is an error, not silent output', () => {
  assert.throws(() => expandTemplate('//@each-texture\nx\n', { lighting: false, slots: [0] }), /without \/\/@end-each/);
  assert.throws(() => buildTargetSource('metal', sampleParsed()), /unknown target/);
});

test('contract: the prelude PerDraw and Lighting members are the contract table, and the table is GpuShaderInterface.hpp', () => {
  const vulkan = fs.readFileSync(path.join(PRELUDE_DIR, 'vulkan.glsl'), 'utf8');
  const block = /uniform PerDraw \{([^}]*)\}/.exec(vulkan)[1];
  const members = [...block.matchAll(/(\w+) (n2_\w+);/g)].map((m) => [m[2], m[1]]);
  assert.deepEqual(members, PER_DRAW.map((m) => [m.name, m.type]));

  const header = fs.readFileSync(path.join(repo, 'renderer/include/renderer/common/GpuShaderInterface.hpp'), 'utf8').replace(/\r\n/g, '\n');
  const table = (name) => {
    const body = new RegExp(`${name}\\{\\{([\\s\\S]*?)\\}\\};`).exec(header)[1];
    return [...body.matchAll(/\{"(\w+)", Block::\w+, (\d+), UniformType::(\w+)/g)].map((m) => ({ name: m[1], offset: Number(m[2]), type: m[3].toLowerCase() }));
  };
  const renamed = (name) => (name === 'uEncodeOutput' ? 'n2_EncodeOutputFlag' : `n2_${name.slice(1)}`);
  assert.deepEqual(table('kPerDraw').map((m) => [renamed(m.name), m.offset]), PER_DRAW.map((m) => [m.name, m.offset]));
  assert.deepEqual(table('kLitLighting').map((m) => [renamed(m.name), m.offset]), LIGHTING.map((m) => [m.name, m.offset]));
  // the first Material sampler binding and the sampler count
  assert.match(header, /kFirstSamplerBinding = 1;/);
  assert.equal(TEXTURE_BINDING_BASE, 1);
  assert.match(header, /kSamplerCount = 5;/);
  assert.match(header, /kLightingBlockSize = 752;/);
  assert.match(header, /kPerDrawBlockSize = 220;/);
});

// ---------------------------------------------------------------------------------------------------------------------
// Reflection and WGSL layout checks (fixtures; no tool needed)
// ---------------------------------------------------------------------------------------------------------------------
function fakeReflection(target, h, layout, mutate = (json) => json) {
  const json = {
    types: {
      _p: { name: 'PerDraw', members: PER_DRAW.map((m) => ({ name: m.name, type: m.type, offset: m.offset })) },
      _m: { name: 'Material', members: layout.members.map((m) => ({ name: m.name, type: m.glsl, offset: m.offset })) },
    },
    inputs: [['n2_UV', 'vec2', 0], ['n2_Color', 'vec4', 1]].map(([name, type, location]) => ({ name, type, location })),
    outputs: [{ type: 'vec4', name: 'n2_FragColor', location: 0 }],
    ubos: [{ type: '_p', name: 'PerDraw', block_size: 220, set: 0, binding: 0 }, { type: '_m', name: 'Material', block_size: layout.size, set: 1, binding: 0 }],
  };
  const textures = h.params.filter((p) => p.type === 'texture');
  if (target === 'vulkan') json.textures = textures.map((p) => ({ type: 'sampler2D', name: `n2_tex${p.slot}`, set: 1, binding: 1 + p.slot }));
  else {
    json.separate_images = textures.map((p) => ({ type: 'texture2D', name: `n2_tex${p.slot}`, set: 1, binding: 1 + p.slot }));
    json.separate_samplers = textures.map((p) => ({ type: 'sampler', name: `n2_samp${p.slot}`, set: 1, binding: 6 + p.slot }));
  }
  return mutate(json);
}

const reflectCase = () => {
  const { header: h } = parseN2Shader(shader(header({ params: [param('u_Tint', 'color'), param('u_Gain', 'float'), param('u_Mask', 'texture')] })));
  return { h, layout: layoutBlock(h.params) };
};

test('reflect: a module that matches the cook passes on both SPIR-V targets', () => {
  const { h, layout } = reflectCase();
  for (const target of ['vulkan', 'webgpu']) assert.deepEqual(checkReflection(fakeReflection(target, h, layout), { target, header: h, layout }), []);
});

// The OpenGL text's uniform names against the SPIR-V reflection (GL cannot run in CI, and the renderer sets uniforms by name)
test('gl uniforms: the names and types of a real GL text match the reflection of the same shader', () => {
  const { h, layout } = reflectCase();
  const reflection = fakeReflection('vulkan', h, layout);
  const gl = buildTargetSource('gl', parseN2Shader(shader(header({ params: [param('u_Tint', 'color'), param('u_Gain', 'float'), param('u_Mask', 'texture')] }))), { path: 'a.n2shader' }).text;
  assert.deepEqual(checkGlUniforms(gl, reflection, { header: h, layout }), []);
  // a renamed uniform, a changed type, a missing one and an unknown extra are each reported
  const messages = (text) => checkGlUniforms(text, reflection, { header: h, layout }).map((d) => d.message).join('\n');
  assert.match(messages(gl.replace('uniform mat4 n2_Model;', 'uniform mat4 n2_ModelMatrix;')), /n2_Model.*not declared|n2_ModelMatrix/);
  assert.match(messages(gl.replace('uniform float u_Gain;', 'uniform int u_Gain;')), /u_Gain.*int.*float/);
  assert.match(messages(gl.replace('uniform float n2_TargetHeight;', '')), /n2_TargetHeight/);
  assert.match(messages(gl.replace('uniform float u_Gain;', 'uniform float u_Gain;\nuniform float u_Extra;')), /u_Extra/);
  assert.match(messages(gl.replace('uniform sampler2D n2_tex0;', '')), /n2_tex0/);
});

test('gl uniforms: the lighting uniforms follow the reflected Lighting block, with the flag renamed', () => {
  const { header: h } = parseN2Shader(shader(header({ params: [param('u_Scale', 'float')], uses: ['lighting'] })));
  const layout = layoutBlock(h.params);
  const reflection = fakeReflection('vulkan', h, layout, (json) => {
    json.types._l = { name: 'Lighting', members: LIGHTING.map((m) => ({ name: m.name, type: m.type, offset: m.offset })) };
    json.ubos.push({ type: '_l', name: 'Lighting', block_size: 752, set: 0, binding: 1 });
    return json;
  });
  const gl = buildTargetSource('gl', parseN2Shader(shader(header({ params: [param('u_Scale', 'float')], uses: ['lighting'] }))), { path: 'a.n2shader' }).text;
  assert.deepEqual(checkGlUniforms(gl, reflection, { header: h, layout }), []);
  assert.match(checkGlUniforms(gl.replace('uniform vec3 n2_AmbientLight;', ''), reflection, { header: h, layout }).map((d) => d.message).join('\n'), /n2_AmbientLight/);
});

test('reflect: an offset glslang assigned differently from the cook is reported with both numbers', () => {
  const { h, layout } = reflectCase();
  const json = fakeReflection('vulkan', h, layout, (j) => { j.types._m.members[1].offset = 24; return j; });
  const found = checkReflection(json, { target: 'vulkan', header: h, layout });
  assert.equal(found.length, 1);
  assert.match(found[0].message, /Material\.u_Gain: glslang put it at offset 24, the cook computed 16/);
});

test('reflect: PerDraw drift, unexpected blocks, bindings, forbidden resources and outputs are reported', () => {
  const { h, layout } = reflectCase();
  const check = (mutate, target = 'vulkan') => checkReflection(fakeReflection(target, h, layout, mutate), { target, header: h, layout }).map((d) => d.message).join('\n');
  assert.match(check((j) => { j.types._p.members[3].offset = 200; return j; }), /PerDraw\.n2_StageDim: glslang put it at offset 200/);
  assert.match(check((j) => { j.ubos.push({ type: '_m', name: 'Extra', block_size: 16, set: 1, binding: 3 }); return j; }), /unexpected uniform block "Extra"/);
  assert.match(check((j) => { j.ssbos = [{ name: 'B' }]; return j; }), /ssbos/);
  assert.match(check((j) => { j.inputs.push({ name: 'v_extra', type: 'vec2', location: 9 }); return j; }), /unexpected input "v_extra"/);
  assert.match(check((j) => { j.outputs.push({ name: 'second', type: 'vec4', location: 1 }); return j; }), /exactly one vec4 output/);
  assert.match(check((j) => { j.textures[0].binding = 4; return j; }), /texture slot 0 \("u_Mask"\) is not a sampler2D at set 1, binding 1/);
  assert.match(check((j) => { j.ubos = j.ubos.slice(1); return j; }), /PerDraw block .* is missing/);
  assert.match(check((j) => { j.textures = [{ type: 'sampler2D', name: 'n2_tex0', set: 1, binding: 1 }]; return j; }, 'webgpu'), /combined sampler2D/);
});

test('reflect: the Lighting block is checked against the 752-byte contract when asked for', () => {
  const { header: h } = parseN2Shader(shader(header({ uses: ['lighting'] })));
  const layout = layoutBlock(h.params);
  const lighting = { name: 'Lighting', type: '_l', block_size: 752, set: 0, binding: 1 };
  const types = { _l: { name: 'Lighting', members: LIGHTING.map((m) => ({ name: m.name, type: m.type, offset: m.offset })) } };
  const json = (extra = {}) => ({ types: { ...types, _p: { members: PER_DRAW.map((m) => ({ name: m.name, type: m.type, offset: m.offset })) } }, inputs: [], outputs: [{ type: 'vec4', name: 'n2_FragColor', location: 0 }], ubos: [{ type: '_p', name: 'PerDraw', block_size: 220, set: 0, binding: 0 }, lighting], ...extra });
  assert.deepEqual(checkReflection(json(), { target: 'vulkan', header: h, layout }), []);
  assert.match(checkReflection(json({ ubos: [{ type: '_p', name: 'PerDraw', block_size: 220, set: 0, binding: 0 }] }), { target: 'vulkan', header: h, layout })[0].message, /Lighting block .* is missing/);
});

test('capabilities: the allow-list', () => {
  assert.deepEqual(checkCapabilities('OpCapability Shader\nOpCapability ImageQuery\n', 'vulkan'), []);
  const found = checkCapabilities('OpCapability Shader\nOpCapability Int64\nOpCapability VulkanMemoryModel\n', 'vulkan');
  assert.equal(found.length, 2);
  assert.match(found[0].message, /Int64/);
});

test('wgsl layout: the Material struct offsets and bindings are checked', () => {
  const { h, layout } = reflectCase();
  const good = `@group(1u) @binding(1u) var n2_tex0 : texture_2d<f32>;
@group(1u) @binding(6u) var n2_samp0 : sampler;
struct Material {
  u_Tint : vec4<f32>,
  u_Gain : f32,
}
@group(1u) @binding(0u) var<uniform> v : Material;
`;
  assert.deepEqual(checkWgslLayout(good, { header: h, layout }), []);
  assert.match(checkWgslLayout(good.replace('u_Gain : f32,', 'pad : vec4<f32>,\n  u_Gain : f32,'), { header: h, layout })[0].message, /Material member 1 is "pad"/);
  assert.match(checkWgslLayout(good.replace('@binding(6u)', '@binding(7u)'), { header: h, layout })[0].message, /sampler slot 0 must be @group\(1\) @binding\(6\)/);
  assert.match(checkWgslLayout(good.replace('@group(1u) @binding(0u) var<uniform>', '@group(1u) @binding(2u) var<uniform>'), { header: h, layout })[0].message, /Material uniform must be at @group\(1\) @binding\(0\)/);
  assert.match(checkWgslLayout('struct Other { a : f32, }', { header: h, layout })[0].message, /no struct starting with the Material member "u_Tint"/);
  // a vec3 member: the WGSL offsets are computed with vec3's 16-byte alignment, as std140 does
  const vec3Header = parseN2Shader(shader(header({ params: [param('u_A', 'float'), param('u_B', 'vec3'), param('u_C', 'float')] }))).header;
  const vec3Layout = layoutBlock(vec3Header.params);
  const wgsl = 'struct Material {\n  u_A : f32,\n  u_B : vec3<f32>,\n  u_C : f32,\n}\n@group(1u) @binding(0u) var<uniform> v : Material;';
  assert.deepEqual(checkWgslLayout(wgsl, { header: vec3Header, layout: vec3Layout }), []);
});

test('glslang output: author lines map to the file and line, prelude lines are labelled', () => {
  const output = [
    'vulkan.frag',
    "ERROR: res://shaders/bad.n2shader:40: 'undefined_fn' : no matching overloaded function found",
    "ERROR: vulkan.frag:12: 'x' : undeclared identifier",
    "WARNING: res://shaders/bad.n2shader:41: 'y' : something",
    'ERROR: 2 compilation errors.  No code generated.',
  ].join('\n');
  const found = parseGlslangOutput(output, { target: 'vulkan', userPath: 'res://shaders/bad.n2shader', inputName: 'vulkan.frag' });
  assert.equal(found.length, 3);
  assert.deepEqual([found[0].file, found[0].line, found[0].severity], ['res://shaders/bad.n2shader', 40, 'error']);
  assert.match(found[0].message, /undefined_fn/);
  assert.deepEqual([found[1].file, found[1].line], ['prelude:vulkan', 12]);
  assert.equal(found[2].severity, 'warning');
  const gl = parseGlslangOutput("ERROR: 1:7: 'z' : bad", { target: 'gl', userPath: 'a.n2shader', inputName: 'gl.frag' });
  assert.deepEqual([gl[0].file, gl[0].line], ['a.n2shader', 7]);
});

// ---------------------------------------------------------------------------------------------------------------------
// The cook without tools
// ---------------------------------------------------------------------------------------------------------------------
const spikeText = fs.readFileSync(path.join(corpusDir, 'spike-user-style.n2shader'), 'utf8');

test('cook: with no tool installed the stages are skipped with a message, and the GL payload and manifest still come out', () => {
  const result = cookShader(spikeText, { path: 'spike.n2shader', tools: noTools });
  assert.equal(result.ok, true);
  assert.ok(result.skipped.length >= 3);
  assert.match(result.skipped[0].reason, /skipped for \w+: \w[\w-]* is not installed/);
  assert.ok(result.outputs.gl.data.length > 0);
  assert.equal(result.outputs.vulkan.data, null);
  assert.deepEqual(result.manifest.payloads.gl.file, 'shader.gl.frag');
  assert.equal(result.manifest.blockSize, 48);
  assert.deepEqual(result.manifest.params.map((p) => [p.name, p.offset ?? `slot${p.slot}`]), [['u_Tint', 0], ['u_Threshold', 16], ['u_Steps', 20], ['u_Offset', 32], ['u_Invert', 44], ['u_Mask', 'slot0']]);
});

test('cook: --require-tools turns a missing tool into an error', () => {
  const result = cookShader(spikeText, { path: 'spike.n2shader', tools: noTools, requireTools: true });
  assert.equal(result.ok, false);
  assert.equal(result.failedStage, 'tools');
  assert.match(result.diagnostics[0].message, /not installed/);
});

test('cook: parse and lint failures stop before any tool, with file:line', () => {
  const lintFail = cookShader(shader(header(), 'void main() {\n  bool b = isnan(1.0);\n}'), { path: 'a.n2shader', tools: noTools });
  assert.equal(lintFail.failedStage, 'lint');
  assert.deepEqual([lintFail.diagnostics[0].file, lintFail.diagnostics[0].line], ['a.n2shader', 4]);
  const parseFail = cookShader('{ "n2shader": 1 }\n#stage fragment\nvoid main() {}\n', { path: 'b.n2shader', tools: noTools });
  assert.equal(parseFail.failedStage, 'parse');
  assert.equal(parseFail.diagnostics[0].file, 'b.n2shader');
});

test('cook: the same input gives the same bytes, whatever the line endings and working directory', () => {
  const a = cookShader(spikeText, { path: 'one/spike.n2shader', tools: noTools });
  const b = cookShader(spikeText.replace(/\n/g, '\r\n'), { path: 'two/spike.n2shader', tools: noTools });
  assert.ok(manifestBytes(a.manifest).equals(manifestBytes(b.manifest)));
  assert.ok(a.outputs.gl.data.equals(b.outputs.gl.data), 'the GL text cites no path (#line uses a source string number)');
});

test('cook: manifest bytes are fixed (key order, no timestamp), and the source hash covers the normalised text', () => {
  const result = cookShader(spikeText, { path: 'spike.n2shader', tools: noTools });
  const text = manifestBytes(result.manifest).toString();
  assert.ok(text.endsWith('}\n'));
  assert.deepEqual(Object.keys(result.manifest), ['formatVersion', 'contract', 'name', 'source', 'uses', 'state', 'blockSize', 'params', 'textures', 'vertexAttributes', 'lint', 'payloads', 'validated', 'tools']);
  assert.doesNotMatch(text, /20\d\d-\d\d-\d\d/);
  assert.match(result.manifest.source.sha256, /^[0-9a-f]{64}$/);
});

test('cook: writeOutputs writes sorted files and refuses a failed cook', () => {
  const dir = tmp('n2shader-out-');
  const written = writeOutputs(cookShader(spikeText, { path: 'spike.n2shader', tools: noTools }), dir);
  assert.deepEqual(written, ['manifest.json', 'shader.gl.frag']);
  assert.deepEqual(fs.readdirSync(dir).sort(), written);
  assert.throws(() => writeOutputs(cookShader('x', { tools: noTools }), dir), /the cook failed/);
});

// ---------------------------------------------------------------------------------------------------------------------
// Tools: discovery, and the stages that need them
// ---------------------------------------------------------------------------------------------------------------------
test('tools: a variable that names a missing file finds nothing, and PATH is searched', () => {
  assert.equal(findTool('tint', { env: { TINT: path.join(tmp('n2shader-none-'), 'tint'), PATH: '' }, platform: process.platform }), null);
  const dir = tmp('n2shader-bin-');
  const name = process.platform === 'win32' ? 'naga.exe' : 'naga';
  fs.writeFileSync(path.join(dir, name), '');
  assert.equal(findTool('naga', { env: { PATH: dir }, platform: process.platform }), path.join(dir, name));
  assert.equal(findTool('naga', { env: { NAGA: path.join(dir, name), PATH: '' }, platform: process.platform }), path.resolve(path.join(dir, name)));
});

const haveSpirv = realTools.glslang && realTools.spirvVal && realTools.spirvCross;
const needs = (tools, why) => ({ skip: tools.every(Boolean) ? false : `${why} not installed` });

test('glslang: an error in the author code cites the author file and line', needs([realTools.glslang], 'glslangValidator'), () => {
  const text = shader(header(), 'void main() {\n  vec4 c = vec4(1.0);\n  c = undefined_fn(c);\n  n2_FragColor = c;\n}');
  const result = cookShader(text, { path: 'res://shaders/bad.n2shader', tools: { ...noTools, glslang: realTools.glslang } });
  assert.equal(result.failedStage, 'glslang');
  const error = result.diagnostics.find((d) => d.severity === 'error');
  assert.deepEqual([error.file, error.line], ['res://shaders/bad.n2shader', 5]);
  assert.match(error.message, /undefined_fn/);
});

test('cook: Vulkan and WebGPU SPIR-V with the reflection self-check, deterministic', needs([haveSpirv], 'glslang, spirv-val and spirv-cross'), () => {
  const tools = { ...noTools, glslang: realTools.glslang, spirvVal: realTools.spirvVal, spirvDis: realTools.spirvDis, spirvCross: realTools.spirvCross };
  const a = cookShader(spikeText, { path: 'spike.n2shader', tools });
  const b = cookShader(spikeText, { path: 'spike.n2shader', tools });
  assert.equal(a.ok, true, JSON.stringify(a.diagnostics));
  assert.ok(a.manifest.validated.vulkan.includes('reflect') && a.manifest.validated.webgpu.includes('reflect'));
  assert.ok(a.outputs.vulkan.data.equals(b.outputs.vulkan.data));
  assert.ok(manifestBytes(a.manifest).equals(manifestBytes(b.manifest)));
  assert.equal(a.manifest.payloads.vulkan.sha256, b.manifest.payloads.vulkan.sha256);
});

test('cook: WebGPU through Tint keeps the contract bindings and the std140 layout', needs([haveSpirv, realTools.tint], 'glslang, spirv-cross and tint'), () => {
  const result = cookShader(spikeText, { path: 'spike.n2shader', tools: realTools });
  assert.equal(result.ok, true, JSON.stringify(result.diagnostics));
  const wgsl = result.outputs.webgpu.data.toString();
  assert.match(wgsl, /@group\(1u?\) @binding\(1u?\) var n2_tex0 : texture_2d<f32>;/);
  assert.match(wgsl, /@group\(1u?\) @binding\(6u?\) var n2_samp0 : sampler;/);
  assert.ok(result.manifest.validated.webgpu.includes('wgsl-layout'));
});

test('cook: the Tint-only negatives are rejected by Tint', needs([haveSpirv, realTools.tint], 'glslang, spirv-cross and tint'), () => {
  const isnan = fs.readFileSync(path.join(corpusDir, 'negative-isnan.n2shader'), 'utf8');
  assert.equal(cookShader(isnan, { path: 'n.n2shader', tools: realTools, lint: false }).failedStage, 'tint');
  const sample = fs.readFileSync(path.join(corpusDir, 'negative-non-uniform-sample.n2shader'), 'utf8');
  const result = cookShader(sample, { path: 'n.n2shader', tools: realTools });
  assert.equal(result.failedStage, 'tint');
  assert.match(result.diagnostics.at(-1).message, /uniform control flow/);
});

// ---------------------------------------------------------------------------------------------------------------------
// The corpus and the CLI
// ---------------------------------------------------------------------------------------------------------------------
test('corpus: every file is listed, and the whole corpus passes with the installed tools (lint and parse stages need none)', () => {
  const result = runCorpus(corpusDir, { tools: realTools });
  assert.equal(result.ok, true, result.lines.filter((l) => l.includes('FAIL')).join('\n'));
  assert.ok(result.counts.positive >= 12);
  // the negatives that only a compiler can reject count as met when the tools are there, as skipped when they are not
  assert.ok(result.counts.negative + result.counts.skipped >= 9);
  const spec = JSON.parse(fs.readFileSync(path.join(corpusDir, 'expect.json'), 'utf8'));
  // one positive per row of the probe table (proposal 1.2) plus the spike's user-style shader
  for (const row of ['inverse-mat3', 'inverse-mat4', 'float-array', 'derivatives', 'texel-fetch-lod', 'texture-size', 'switch', 'frag-coord', 'uint-hash', 'while-loop', 'spike-user-style']) {
    assert.ok(spec.positive.some((entry) => entry.file === `${row}.n2shader`), row);
  }
});

test('corpus: an unlisted file and a wrongly accepted negative fail the run', () => {
  const dir = tmp('n2shader-corpus-');
  fs.writeFileSync(path.join(dir, 'ok.n2shader'), shader(header()));
  fs.writeFileSync(path.join(dir, 'unlisted.n2shader'), shader(header()));
  fs.writeFileSync(path.join(dir, 'bad.n2shader'), shader(header()));
  fs.writeFileSync(path.join(dir, 'expect.json'), JSON.stringify({
    positive: [{ file: 'ok.n2shader' }],
    negative: [{ file: 'bad.n2shader', expect: [{ stage: 'lint', match: 'x' }] }],
  }));
  const result = runCorpus(dir, { tools: noTools, targets: [] });
  assert.equal(result.ok, false);
  assert.equal(result.counts.failed, 2);
  assert.match(result.lines.join('\n'), /unlisted\.n2shader is in the corpus folder but not in expect\.json/);
  assert.match(result.lines.join('\n'), /ACCEPTED an invalid shader/);
});

test('corpus: a negative that fails at another stage or for another reason fails the run', () => {
  const dir = tmp('n2shader-corpus-');
  fs.writeFileSync(path.join(dir, 'a.n2shader'), shader(header(), 'void main() { bool b = isnan(1.0); }'));
  fs.writeFileSync(path.join(dir, 'expect.json'), JSON.stringify({ negative: [
    { file: 'a.n2shader', expect: [{ stage: 'tint', match: 'isnan' }] },
    { file: 'a.n2shader', expect: [{ stage: 'lint', match: 'no such text' }] },
  ] }));
  const result = runCorpus(dir, { tools: noTools });
  assert.equal(result.counts.failed, 2);
  assert.match(result.lines.join('\n'), /rejected at lint, expected tint/);
  assert.match(result.lines.join('\n'), /no message matches/);
});

test('cli: argument parsing', () => {
  assert.deepEqual(parseArguments(['cook', 'a.n2shader', '--out', 'x', '--targets', 'vulkan,gl', '--relaxed']), {
    command: 'cook', positional: ['a.n2shader'], out: 'x', targets: ['vulkan', 'gl'], relaxed: true, lint: true, requireTools: false, requirePayload: false, cite: null, target: null,
  });
  assert.throws(() => parseArguments(['cook', '--targets', 'metal']), /unknown target/);
  assert.throws(() => parseArguments(['cook', '--bogus']), /unknown option/);
  assert.throws(() => parseArguments(['cook', '--out']), /needs a value/);
});

test('cli: exit codes and output', () => {
  const out = [];
  const err = [];
  const io = { stdout: (t) => out.push(t), stderr: (t) => err.push(t) };
  const dir = tmp('n2shader-cli-');
  const good = path.join(dir, 'good.n2shader');
  fs.writeFileSync(good, spikeText);
  const bad = path.join(dir, 'bad.n2shader');
  fs.writeFileSync(bad, shader(header(), 'void main() {\n  gl_Position = vec4(0);\n}'));
  assert.equal(main(['check', bad], io), 1);
  assert.match(err.join(''), /bad\.n2shader:4: error: \[lint\] gl_Position/);
  assert.equal(main(['prelude', good, '--target', 'webgpu'], io), 0);
  assert.match(out.join(''), /uniform texture2D n2_tex0;/);
  assert.equal(main(['nope'], io), 2);
  assert.equal(main([], io), 2);
  assert.equal(main(['check'], io), 2);
  assert.equal(main(['check', path.join(dir, 'missing.n2shader')], io), 2);
  assert.equal(main(['prelude', good], io), 2);
  assert.equal(main(['tools'], io), 0);
  assert.equal(main(['cook', good, '--out', path.join(dir, 'out'), '--targets', 'gl'], io), 0);
  assert.ok(fs.existsSync(path.join(dir, 'out', 'shader.gl.frag')));
  assert.ok(fs.existsSync(path.join(dir, 'out', 'manifest.json')));
});

// ---------------------------------------------------------------------------------------------------------------------
// The cooked fixtures of the renderer's user-shader tests (tests/renderer/fixtures/user-shaders/, regenerated by
// scripts/cook-user-shader-fixtures.sh). Not compared byte for byte (the SPIR-V depends on the glslang version): the
// committed manifest must still describe what the source cooks to, and the payload must be a SPIR-V module of the size it names.
// ---------------------------------------------------------------------------------------------------------------------
test('renderer fixtures: every committed manifest matches its .n2shader source', () => {
  const dir = path.join(repo, 'tests/renderer/fixtures/user-shaders');
  const sources = fs.readdirSync(dir).filter((file) => file.endsWith('.n2shader')).sort();
  assert.ok(sources.length >= 5, 'the fixtures the renderer tests load');
  for (const file of sources) {
    const name = file.replace(/\.n2shader$/, '');
    const result = cookShader(fs.readFileSync(path.join(dir, file), 'utf8'), { path: `tests/renderer/fixtures/user-shaders/${file}`, targets: ['vulkan', 'webgpu', 'gl'], tools: noTools });
    assert.ok(result.ok, `${file}: ${result.diagnostics.map((d) => d.message).join('; ')}`);
    const committed = JSON.parse(fs.readFileSync(path.join(dir, name, 'manifest.json'), 'utf8'));
    for (const key of ['formatVersion', 'contract', 'name', 'source', 'uses', 'state', 'blockSize', 'params', 'textures', 'vertexAttributes', 'lint']) {
      assert.deepEqual(committed[key], JSON.parse(JSON.stringify(result.manifest[key])), `${file}: "${key}" is stale; run scripts/cook-user-shader-fixtures.sh`);
    }
    const spirv = fs.readFileSync(path.join(dir, name, committed.payloads.vulkan.file));
    assert.equal(spirv.readUInt32LE(0), 0x07230203, `${file}: the payload is not SPIR-V`);
    assert.equal(spirv.length, committed.payloads.vulkan.bytes, `${file}: the payload size differs from the manifest`);
    // the WebGPU payload: the WGSL Tint wrote, whose entry point the renderer names (main) and whose size the manifest records
    const wgsl = fs.readFileSync(path.join(dir, name, committed.payloads.webgpu.file));
    assert.equal(Buffer.byteLength(wgsl.toString('utf8').replace(/\r\n/g, '\n')), committed.payloads.webgpu.bytes, `${file}: the WGSL size differs from the manifest`);
    assert.match(wgsl.toString('utf8'), /@fragment\s+fn main\(/, `${file}: the WGSL has no fragment entry point main`);
    // the OpenGL payload is plain generated text, not tool output: byte for byte what the cook makes (LF)
    const gl = fs.readFileSync(path.join(dir, name, committed.payloads.gl.file), 'utf8').replace(/\r\n/g, '\n');
    assert.equal(gl, result.outputs.gl.data.toString('utf8'), `${file}: shader.gl.frag is stale; run scripts/cook-user-shader-fixtures.sh`);
    assert.equal(Buffer.byteLength(gl), committed.payloads.gl.bytes, `${file}: the GL size differs from the manifest`);
    assert.ok(committed.validated.gl.includes('gl-uniforms'), `${file}: the GL uniform names were not checked against the reflection`);
  }
});
