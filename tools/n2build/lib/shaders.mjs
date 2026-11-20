// The shader step of the cook (user shaders, stage 6; docs/packs.html): every .n2shader under assets/ is cooked with
// tools/n2shader into .import/shaders/<uuid>/ (the folder the engine's Shader::MakeBlob reads), exactly the files and bytes
// `n2shader cook` writes, with only the payloads the target's renderers use. Sorted names, no timestamps: the same project
// and tools make the same pack. A stripped pack keeps the header of each .n2shader but not its source, and leaves the OpenGL
// text out (it is readable source); such a pack cannot recompile a shader.
import fs from 'node:fs';
import path from 'node:path';

import { cookShader, formatDiagnostic, manifestBytes, MANIFEST_FILE } from '../../n2shader/lib/cook.mjs';
import { discoverTools } from '../../n2shader/lib/tools.mjs';
import { BuildError } from './report.mjs';

export const SHADER_EXTENSION = '.n2shader';
export const SHADER_PACK_FOLDER = '.import/shaders';

/** The shader payloads a package kind's renderers use (a descriptor's `shaderTargets` overrides this) */
export function shaderTargetsFor(descriptor) {
  if (Array.isArray(descriptor?.shaderTargets)) return descriptor.shaderTargets;
  switch (descriptor?.package?.kind) {
    case 'web': return ['webgpu'];
    case 'android':
    case 'ios': return ['vulkan'];
    default: return ['vulkan', 'gl'];
  }
}

/** The project's .n2shader files as {name ('/'-separated, under assets/), file}, sorted by name */
export function findShaderSources(projectDir) {
  const out = [];
  const walk = (dir, prefix) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))) {
      const name = `${prefix}/${entry.name}`;
      if (entry.isDirectory()) walk(path.join(dir, entry.name), name);
      else if (entry.isFile() && entry.name.toLowerCase().endsWith(SHADER_EXTENSION)) out.push({ name, file: path.join(dir, entry.name) });
    }
  };
  const assets = path.join(projectDir, 'assets');
  if (fs.existsSync(assets) && fs.statSync(assets).isDirectory()) walk(assets, 'assets');
  return out;
}

/** The asset's UUID from its .meta (.import/<path under assets/>.meta, as ResourceLoader::MetaPathOf); null when there is none */
export function readShaderUuid(projectDir, name) {
  const meta = path.join(projectDir, '.import', ...name.split('/').slice(1)) + '.meta';
  try {
    const uuid = JSON.parse(fs.readFileSync(meta, 'utf8')).uuid;
    return typeof uuid === 'string' && /^[0-9a-fA-F-]{8,64}$/.test(uuid) ? uuid : null;
  } catch {
    return null;
  }
}

/** The tools a cook for these targets cannot do without, with what to install when one is missing */
export function missingShaderTools(tools, targets) {
  const need = [['glslang', 'glslangValidator (the Vulkan SDK, or apt install glslang-tools)'],
    ['spirvVal', 'spirv-val (the Vulkan SDK, or apt install spirv-tools)'],
    ['spirvDis', 'spirv-dis (the Vulkan SDK, or apt install spirv-tools)'],
    ['spirvCross', 'spirv-cross (the Vulkan SDK, or apt install spirv-cross)']];
  if (targets.includes('webgpu')) need.push(['tint', 'tint (n2build downloads the pinned one; or set TINT)']);
  return need.filter(([name]) => !tools[name]).map(([, how]) => how);
}

/** The header and the `#stage fragment` line of a .n2shader, with an empty stage (what a stripped pack holds) */
export function stripSource(text) {
  const match = /^[ \t]*#stage fragment[ \t]*\r?$/m.exec(text);
  if (!match) return text; // not a shader the cook accepts; the cook has already reported it
  return `${text.slice(0, match.index)}#stage fragment\n`;
}

/**
 * Cooks every shader of the project. Returns { entries: [{name, data}] (the pack entries under .import/shaders/),
 * replaced: Map(name -> data) (stripped .n2shader files), count }. Throws a BuildError naming the shader and the reason.
 */
export function cookProjectShaders({ projectDir, targets, strip = false, env = process.env, tools = null, log = () => {} }) {
  const sources = findShaderSources(projectDir);
  const result = { entries: [], replaced: new Map(), count: sources.length };
  if (sources.length === 0) return result;
  const wanted = strip ? targets.filter((target) => target !== 'gl') : targets;
  const found = tools ?? discoverTools({ env });
  const missing = missingShaderTools(found, wanted);
  if (missing.length > 0) {
    throw new BuildError(`The project has ${sources.length} shader(s) (.n2shader) and cooking them needs:\n${missing.map((m) => `  ${m}`).join('\n')}`);
  }
  const seen = new Map();
  for (const { name, file } of sources) {
    const uuid = readShaderUuid(projectDir, name);
    if (!uuid) throw new BuildError(`${name} has no .meta with an id (.import/${name.slice('assets/'.length)}.meta): open the project in the editor once so the asset gets one`);
    if (seen.has(uuid)) throw new BuildError(`${name} and ${seen.get(uuid)} have the same asset id ${uuid}`);
    seen.set(uuid, name);
    const text = fs.readFileSync(file, 'utf8');
    const cooked = cookShader(text, { path: name, targets: wanted, tools: found, requireTools: false });
    for (const diagnostic of cooked.diagnostics) if (diagnostic.severity !== 'error') log(formatDiagnostic(diagnostic));
    if (!cooked.ok) {
      throw new BuildError(`${name} did not cook:\n${cooked.diagnostics.filter((d) => d.severity === 'error').map((d) => `  ${formatDiagnostic(d)}`).join('\n')}`);
    }
    const dir = `${SHADER_PACK_FOLDER}/${uuid}`;
    for (const target of Object.keys(cooked.outputs)) {
      const output = cooked.outputs[target];
      if (!output?.data) throw new BuildError(`${name}: no ${target} payload was made (${cooked.skipped.map((s) => s.reason).join('; ') || 'unknown reason'})`);
      result.entries.push({ name: `${dir}/${output.file}`, data: output.data });
    }
    result.entries.push({ name: `${dir}/${MANIFEST_FILE}`, data: manifestBytes(cooked.manifest) });
    if (strip) result.replaced.set(name, Buffer.from(stripSource(text)));
  }
  result.entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return result;
}
