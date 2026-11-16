// Reads a project's list of engine modules ("build.modules" in project.n2proj), scans the project for what it uses, and reports
// where they disagree (docs/project-modules.html). A build with a list runs this, so a game cannot ship without something it needs.
//
// Usage: node scripts/project-modules.mjs <project folder> [--target T] [--suggest] [--json]
//   --target T  the list a build for target T uses: build.targets.T.modules laid over build.modules, key by key.
//   default     prints what the project uses against its list. Exit 0: nothing contradicts the list. Exit 1: something does.
//   --suggest   prints the smallest list the scan can justify, as the "build" block to paste into project.n2proj.
//   --json      the report as JSON instead of text.
//   Exit 2: wrong usage, or a project that cannot be read.
//
// The scan is a helper, not a proof. Scenes and prefabs name their components, so those are certain. Lua can reach a component
// through a computed name, as in AddComponent(kinds[i]), which no scan can resolve: those lines are reported as "could not be
// checked", and they are why the list stays explicit and why nothing is ever stripped automatically.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

/** What each module is, as far as a project can show it. Lua globals and handlers are matched as whole words. */
export const MODULES = {
  physics3d: {
    components: ['Rigidbody', 'BoxCollider', 'SphereCollider', 'CapsuleCollider', 'CharacterController'],
    luaPatterns: [/\bPhysics\.[A-Za-z]\w*/, /\bOn(Collision|Trigger)(Enter|Stay|Exit)\b/, /\bBodyType\./],
  },
  physics2d: {
    components: ['Rigidbody2D', 'BoxCollider2D', 'CircleCollider2D', 'PolygonCollider2D'],
    luaPatterns: [/\bPhysics2D\.[A-Za-z]\w*/, /\bOn(Collision|Trigger)(Enter|Stay|Exit)2D\b/, /\bBodyType2D\./],
  },
  // Lua reaches these only through the component (no global of their own), so its name is the whole of what a script shows
  particles: {
    components: ['ParticleSystem'],
    luaPatterns: [],
  },
  softbodies: {
    components: ['SoftBody'],
    luaPatterns: [],
  },
  // AudioListener is not a use: a listener with no sound to hear does nothing, so a scene that only has one (on its camera)
  // can still leave audio out. Audio.PlayOneShot and the other Audio functions are.
  audio: {
    components: ['AudioSource'],
    luaPatterns: [/\bAudio\.[A-Za-z]\w*/],
  },
  ui: {
    components: ['Canvas', 'RectTransform', 'Image', 'UIText', 'Button'],
    luaPatterns: [/\bUI\.[A-Za-z]\w*/],
  },
  // Scripting is the Lua runtime itself: any .lua file under assets or scenes is a use (scanProject adds it), and so is a
  // LuaComponent in a scene or prefab (a scene saved with scripts still loads without the module, but its scripts do not run)
  scripting: {
    components: ['LuaComponent'],
    luaPatterns: [],
  },
  // The sessions (server, client, lockstep, rollback) and the Network Lua table. A project whose settings pick a network
  // preset also needs them (scanProject adds that use). The components keep their data without the module, but do nothing.
  network: {
    components: ['NetworkObject', 'NetworkTransform', 'LagCompensatedHitbox'],
    luaPatterns: [/(?<![\w.])Network\.[A-Za-z]\w*/, /\bPhysics\.(Raycast|SphereCast)LagCompensated\b/],
  },
};

// The renderer backends are keys of the list too, but not modules a scene can use: a game picks its renderer at run time
// (--renderer) or, for a build target, with build.targets.<T>.renderer, and the scan reads that one (see findConflicts)
// "vulkan": false leaves the Vulkan renderer out of a target that would build it; true changes nothing (the target is the ceiling)
export const RENDERER_MODULES = ['vulkan', 'webgpu'];
for (const renderer of RENDERER_MODULES) {
  MODULES[renderer] = { components: [], luaPatterns: [] };
}

export const PHYSICS3D_VALUES = ['none', 'physx', 'jolt', 'both'];
/** The modules a list turns on (true) or off (false) */
export const BOOLEAN_MODULES = ['physics2d', 'particles', 'softbodies', 'audio', 'ui', 'scripting', 'network'];

const SCENE_EXTENSIONS = new Set(['.scene', '.prefab', '.json']);
const SKIPPED_FOLDERS = new Set(['.import', '.n2', '.git', 'node_modules', 'build']);

/** The text of a Lua file with its comments removed and its strings kept, so a commented-out call does not count as a use. */
export function stripLuaComments(source) {
  let out = '';
  let i = 0;
  const longBracket = (at) => {
    const match = /^\[(=*)\[/.exec(source.slice(at, at + 64));
    return match ? match[1].length : -1;
  };
  while (i < source.length) {
    const c = source[i];
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < source.length && source[j] !== c && source[j] !== '\n') {
        j += source[j] === '\\' ? 2 : 1;
      }
      out += source.slice(i, j + 1);
      i = j + 1;
    } else if (c === '[' && longBracket(i) >= 0) {
      const close = `]${'='.repeat(longBracket(i))}]`;
      const end = source.indexOf(close, i);
      const stop = end < 0 ? source.length : end + close.length;
      out += source.slice(i, stop);
      i = stop;
    } else if (c === '-' && source[i + 1] === '-') {
      const level = longBracket(i + 2);
      if (level >= 0) {
        const end = source.indexOf(`]${'='.repeat(level)}]`, i);
        i = end < 0 ? source.length : end + level + 2;
      } else {
        const end = source.indexOf('\n', i);
        i = end < 0 ? source.length : end;
      }
    } else {
      out += c;
      i += 1;
    }
  }
  return out;
}

function lineOf(text, index) {
  return text.slice(0, index).split('\n').length;
}

/** What one Lua file shows: uses of each module, and component calls whose name is not a literal. */
export function scanLua(source) {
  const text = stripLuaComments(source);
  const uses = [];
  for (const [module, { components, luaPatterns }] of Object.entries(MODULES)) {
    for (const pattern of luaPatterns) {
      const match = pattern.exec(text);
      if (match) {
        uses.push({ module, what: match[0], line: lineOf(text, match.index) });
      }
    }
    for (const component of components) {
      const match = new RegExp(`["']${component}["']`).exec(text);
      if (match) {
        uses.push({ module, what: component, line: lineOf(text, match.index) });
      }
    }
  }
  const unresolved = [];
  for (const match of text.matchAll(/:\s*(?:AddComponent|GetComponent)\s*\(\s*([^"'\s)][^)]*)\)/g)) {
    unresolved.push({ what: match[0].replace(/^:\s*/, ''), line: lineOf(text, match.index) });
  }
  return { uses, unresolved };
}

/** Every component type a scene or prefab document names, wherever it nests */
export function componentTypesIn(document) {
  const found = new Set();
  const walk = (node) => {
    if (Array.isArray(node)) {
      node.forEach(walk);
    } else if (node && typeof node === 'object') {
      if (Array.isArray(node.components)) {
        for (const component of node.components) {
          if (component && typeof component.type === 'string') {
            found.add(component.type);
          }
        }
      }
      Object.values(node).forEach(walk);
    }
  };
  walk(document);
  return found;
}

/** Components that make an object a networked one */
export const NETWORK_COMPONENTS = MODULES.network.components;
/** The 3D and 2D physics components: an object with one of these is simulated by a physics backend */
export const PHYSICS_COMPONENTS = [...MODULES.physics3d.components, ...MODULES.physics2d.components];

/** How many objects of a scene or prefab document hold a networking component and a physics component together */
export function networkedPhysicsObjectsIn(document) {
  let count = 0;
  const walk = (node) => {
    if (Array.isArray(node)) {
      node.forEach(walk);
    } else if (node && typeof node === 'object') {
      if (Array.isArray(node.components)) {
        const types = new Set(node.components.map((component) => component?.type));
        if (NETWORK_COMPONENTS.some((type) => types.has(type)) && PHYSICS_COMPONENTS.some((type) => types.has(type))) {
          count += 1;
        }
      }
      Object.values(node).forEach(walk);
    }
  };
  walk(document);
  return count;
}

/**
 * build.strictFp: true or false when the project says, undefined when it does not. It asks for the strict floating-point
 * profile (N2ENGINE_STRICT_FP: no FP contraction, no fast-math) that peers of a network game need to compute the same floats.
 * Throws on a value that is not a boolean.
 */
export function readStrictFp(project) {
  const value = project?.build?.strictFp;
  if (value !== undefined && typeof value !== 'boolean') {
    throw new Error(`build.strictFp must be true or false, not ${JSON.stringify(value)}`);
  }
  return value;
}

function filesUnder(folder) {
  const files = [];
  if (!fs.existsSync(folder)) {
    return files;
  }
  for (const entry of fs.readdirSync(folder, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
    if (entry.isDirectory()) {
      if (!SKIPPED_FOLDERS.has(entry.name)) {
        files.push(...filesUnder(path.join(folder, entry.name)));
      }
    } else {
      files.push(path.join(folder, entry.name));
    }
  }
  return files;
}

/** Checks one list (build.modules, or a target's modules); where names the key in messages */
function checkList(modules, where) {
  if (!modules || typeof modules !== 'object' || Array.isArray(modules)) {
    throw new Error(`${where} must be an object`);
  }
  for (const key of Object.keys(modules)) {
    if (!(key in MODULES)) {
      throw new Error(`${where}: unknown module "${key}" (known: ${Object.keys(MODULES).join(', ')})`);
    }
  }
  if (modules.physics3d !== undefined && !PHYSICS3D_VALUES.includes(modules.physics3d)) {
    throw new Error(`${where}.physics3d must be one of ${PHYSICS3D_VALUES.join(', ')}, not ${JSON.stringify(modules.physics3d)}`);
  }
  for (const module of [...BOOLEAN_MODULES, ...RENDERER_MODULES]) {
    if (modules[module] !== undefined && typeof modules[module] !== 'boolean') {
      throw new Error(`${where}.${module} must be true or false, not ${JSON.stringify(modules[module])}`);
    }
  }
}

/**
 * Reads the list. Throws on a list that a build could not act on. Absent modules mean "keep it": null for the whole list.
 * With a target, that target's build.targets.<target>.modules is laid over build.modules key by key (build targets stage 3),
 * so a target can change one module and keep the others: { physics3d: "physx" } over { physics3d: "both", physics2d: true }
 * is { physics3d: "physx", physics2d: true }.
 */
export function readModuleList(project, target) {
  const base = project?.build?.modules;
  if (base !== undefined) {
    checkList(base, 'build.modules');
  }
  const targetModules = target ? project?.build?.targets?.[target]?.modules : undefined;
  if (targetModules !== undefined) {
    checkList(targetModules, `build.targets.${target}.modules`);
  }
  if (base === undefined && targetModules === undefined) {
    return null;
  }
  return { ...(base ?? {}), ...(targetModules ?? {}) };
}

/** Scans a project folder. Throws when project.n2proj cannot be read. */
export function scanProject(folder, target) {
  const projectPath = path.join(folder, 'project.n2proj');
  const project = JSON.parse(fs.readFileSync(projectPath, 'utf8'));
  const list = readModuleList(project, target);
  const uses = [];
  const unresolved = [];
  const unreadable = [];
  const networkedPhysics = [];
  const luaNetworkedPhysics = [];

  for (const file of [...filesUnder(path.join(folder, 'assets')), ...filesUnder(path.join(folder, 'scenes'))]) {
    const relative = path.relative(folder, file).split(path.sep).join('/');
    const extension = path.extname(file).toLowerCase();
    if (extension === '.lua') {
      uses.push({ module: 'scripting', what: 'a Lua file', file: relative });
      const result = scanLua(fs.readFileSync(file, 'utf8'));
      uses.push(...result.uses.map((use) => ({ ...use, file: relative })));
      unresolved.push(...result.unresolved.map((item) => ({ ...item, file: relative })));
      // Components added from Lua are not in any scene: a file that names a networking and a physics component is the nearest
      // the scan can get to an object that has both (it cannot tell whether one object gets both)
      const named = new Set(result.uses.map((use) => use.what));
      if (NETWORK_COMPONENTS.some((type) => named.has(type)) && PHYSICS_COMPONENTS.some((type) => named.has(type))) {
        luaNetworkedPhysics.push(relative);
      }
    } else if (SCENE_EXTENSIONS.has(extension)) {
      let document;
      try {
        document = JSON.parse(fs.readFileSync(file, 'utf8'));
      } catch {
        unreadable.push(relative);
        continue;
      }
      const types = componentTypesIn(document);
      const together = networkedPhysicsObjectsIn(document);
      if (together > 0) {
        networkedPhysics.push({ file: relative, objects: together });
      }
      for (const [module, { components }] of Object.entries(MODULES)) {
        for (const component of components) {
          if (types.has(component)) {
            uses.push({ module, what: component, file: relative });
          }
        }
      }
    }
  }

  // What the project's settings ask for from the 3D backend
  const settings = project.settings ?? {};
  const backend = settings.physics?.backend;
  const needsJolt = backend === 'jolt' || settings.network?.preset === 'deterministic';
  const needsPhysx = backend === 'physx';
  // The renderer the build target asks for (build.targets.<T>.renderer): a backend the list leaves out cannot be the one asked for
  const renderer = target ? project.build?.targets?.[target]?.renderer : undefined;
  // Without a startupScene the players run assets/scene.lua in an empty scene, so a game without Lua must name one
  const startsFromLua = typeof project.startupScene !== 'string' || project.startupScene === '';
  const preset = settings.network?.preset;
  if (preset === 'authoritative' || preset === 'deterministic') {
    uses.push({ module: 'network', what: `the network preset "${preset}"`, file: 'project.n2proj' });
  }
  const strictFp = readStrictFp(project);
  return { name: project.name, list, uses, unresolved, unreadable, networkedPhysics, luaNetworkedPhysics, strictFp, needsJolt, needsPhysx, startsFromLua, renderer, target };
}

/**
 * Whether a build of this project gets the strict floating-point profile from the project: build.strictFp when it says, else on
 * for a game that uses networking (a network component, the Network Lua table or a network preset), else null (the build's own
 * default, N2ENGINE_STRICT_FP, decides). n2build passes the answer to CMake.
 */
export function resolveStrictFp(scan) {
  if (scan.strictFp !== undefined) {
    return scan.strictFp;
  }
  return scan.uses.some((use) => use.module === 'network') ? true : null;
}

/**
 * A warning for a project that turned strict floating point off while objects hold a networking component and a physics
 * component: two peers can then step the same body to different floats. null when there is nothing to warn about.
 */
export function strictFpWarning(scan) {
  const fromLua = scan.luaNetworkedPhysics ?? [];
  if (scan.strictFp !== false || (scan.networkedPhysics.length === 0 && fromLua.length === 0)) {
    return null;
  }
  if (scan.networkedPhysics.length === 0) {
    return `build.strictFp is false, but ${fromLua[0]}${fromLua.length > 1 ? ` (and ${fromLua.length - 1} more Lua file${fromLua.length === 2 ? '' : 's'})` : ''} names both a networking component and a physics component: if a script gives one object both, peers built without the strict floating-point profile can compute different physics results and desync. Set build.strictFp to true, or remove it to let the build turn it on`;
  }
  const objects = scan.networkedPhysics.reduce((sum, item) => sum + item.objects, 0);
  const first = scan.networkedPhysics[0].file;
  return `build.strictFp is false, but ${objects} object${objects === 1 ? '' : 's'} (first in ${first}) have both a networking component and a physics component: peers built without the strict floating-point profile can compute different physics results and desync. Set build.strictFp to true, or remove it to let the build turn it on`;
}

/** What contradicts the list: every one of these would misbehave in a game built from it. */
export function findConflicts(scan) {
  const conflicts = [];
  const { list } = scan;
  if (!list) {
    return conflicts;
  }
  const stripped = (module) => (module === 'physics3d' ? list.physics3d === 'none' : list[module] === false);
  for (const use of scan.uses) {
    if (stripped(use.module)) {
      const where = use.line ? `${use.file}:${use.line}` : use.file;
      conflicts.push(`${where} uses ${use.what}, but the list leaves ${use.module} out`);
    }
  }
  if (RENDERER_MODULES.includes(scan.renderer) && list[scan.renderer] === false) {
    conflicts.push(`build.targets.${scan.target}.renderer asks for "${scan.renderer}", but the list leaves ${scan.renderer} out (the software renderer is always built)`);
  }
  if (list.scripting === false && scan.startsFromLua) {
    conflicts.push('the project has no startupScene, so a player runs assets/scene.lua, but the list leaves scripting out: name a startupScene');
  }
  if (list.physics3d !== undefined) {
    if (scan.needsJolt && !['jolt', 'both'].includes(list.physics3d)) {
      conflicts.push(`the project settings need the Jolt backend (physics.backend or the deterministic network preset), but physics3d is "${list.physics3d}"`);
    }
    if (scan.needsPhysx && !['physx', 'both'].includes(list.physics3d)) {
      conflicts.push(`the project settings ask for physics.backend "physx", but physics3d is "${list.physics3d}"`);
    }
  }
  return conflicts;
}

/** The smallest list the scan can justify. It is a starting point, because the scan cannot see computed names. */
export function suggestList(scan) {
  const used = new Set(scan.uses.map((use) => use.module));
  let physics3d = 'none';
  if (scan.needsJolt) {
    physics3d = 'jolt';
  } else if (used.has('physics3d') || scan.needsPhysx) {
    physics3d = 'physx';
  }
  const list = { physics3d };
  for (const module of BOOLEAN_MODULES) {
    list[module] = used.has(module);
  }
  // The scan cannot see a renderer, so what the project's list already says stays
  for (const renderer of RENDERER_MODULES) {
    if (!scan.target && scan.list && typeof scan.list[renderer] === 'boolean') { // with a target, scan.list has the target's keys laid over build.modules: not what to paste into build.modules
      list[renderer] = scan.list[renderer];
    }
  }
  // A project with no startupScene is started from assets/scene.lua, which needs the runtime
  list.scripting = list.scripting || scan.startsFromLua;
  return list;
}

function main(argv) {
  // --target T takes a value; every other option is a flag
  let target;
  const rest = [];
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === '--target' && i + 1 < argv.length) {
      target = argv[++i];
    } else if (argv[i].startsWith('--target=')) {
      target = argv[i].slice('--target='.length);
    } else {
      rest.push(argv[i]);
    }
  }
  const flags = new Set(rest.filter((arg) => arg.startsWith('--')));
  const folders = rest.filter((arg) => !arg.startsWith('--'));
  const known = new Set(['--suggest', '--json']);
  if (folders.length !== 1 || [...flags].some((flag) => !known.has(flag)) || target === '') {
    console.error('usage: project-modules.mjs <project folder> [--target T] [--suggest] [--json]');
    return 2;
  }
  let scan;
  try {
    scan = scanProject(folders[0], target);
  } catch (error) {
    console.error(`project-modules: ${folders[0]}: ${error.message}`);
    return 2;
  }
  const conflicts = findConflicts(scan);

  if (flags.has('--suggest')) {
    console.log(JSON.stringify({ build: { modules: suggestList(scan) } }, null, 2));
    if (scan.unresolved.length > 0) {
      console.error(`Check these by hand: ${scan.unresolved.length} component call(s) with a computed name could need a module the scan cannot see.`);
    }
    return 0;
  }
  if (flags.has('--json')) {
    console.log(JSON.stringify({ ...scan, conflicts, suggested: suggestList(scan) }, null, 2));
    return conflicts.length > 0 ? 1 : 0;
  }

  console.log(`Project: ${scan.name ?? folders[0]}`);
  console.log(scan.list ? `List: ${JSON.stringify(scan.list)}` : 'List: none, so every module is built');
  for (const module of Object.keys(MODULES).filter((name) => !RENDERER_MODULES.includes(name))) {
    const uses = scan.uses.filter((use) => use.module === module);
    console.log(`  ${module}: ${uses.length > 0 ? `used (${uses.length} place${uses.length === 1 ? '' : 's'}; first: ${uses[0].file})` : 'no use found'}`);
  }
  for (const item of scan.unresolved) {
    console.log(`  could not be checked: ${item.file}:${item.line} ${item.what}`);
  }
  for (const file of scan.unreadable) {
    console.log(`  could not be read as JSON: ${file}`);
  }
  for (const conflict of conflicts) {
    console.error(`CONFLICT: ${conflict}`);
  }
  return conflicts.length > 0 ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  process.exitCode = main(process.argv.slice(2));
}
