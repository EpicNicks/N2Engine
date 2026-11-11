// Reads a project's list of engine modules ("build.modules" in project.n2proj), scans the project for what it uses, and reports
// where they disagree (docs/project-modules.html). A build with a list runs this, so a game cannot ship without something it needs.
//
// Usage: node scripts/project-modules.mjs <project folder> [--suggest] [--json]
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
};

export const PHYSICS3D_VALUES = ['none', 'physx', 'jolt', 'both'];
/** The modules a list turns on (true) or off (false) */
export const BOOLEAN_MODULES = ['physics2d', 'particles', 'softbodies'];

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

/** Reads the list. Throws on a list that a build could not act on. Absent modules mean "keep it": null for the whole list. */
export function readModuleList(project) {
  const modules = project?.build?.modules;
  if (modules === undefined) {
    return null;
  }
  if (!modules || typeof modules !== 'object' || Array.isArray(modules)) {
    throw new Error('build.modules must be an object');
  }
  for (const key of Object.keys(modules)) {
    if (!(key in MODULES)) {
      throw new Error(`build.modules: unknown module "${key}" (known: ${Object.keys(MODULES).join(', ')})`);
    }
  }
  if (modules.physics3d !== undefined && !PHYSICS3D_VALUES.includes(modules.physics3d)) {
    throw new Error(`build.modules.physics3d must be one of ${PHYSICS3D_VALUES.join(', ')}, not ${JSON.stringify(modules.physics3d)}`);
  }
  for (const module of BOOLEAN_MODULES) {
    if (modules[module] !== undefined && typeof modules[module] !== 'boolean') {
      throw new Error(`build.modules.${module} must be true or false, not ${JSON.stringify(modules[module])}`);
    }
  }
  return modules;
}

/** Scans a project folder. Throws when project.n2proj cannot be read. */
export function scanProject(folder) {
  const projectPath = path.join(folder, 'project.n2proj');
  const project = JSON.parse(fs.readFileSync(projectPath, 'utf8'));
  const list = readModuleList(project);
  const uses = [];
  const unresolved = [];
  const unreadable = [];

  for (const file of [...filesUnder(path.join(folder, 'assets')), ...filesUnder(path.join(folder, 'scenes'))]) {
    const relative = path.relative(folder, file).split(path.sep).join('/');
    const extension = path.extname(file).toLowerCase();
    if (extension === '.lua') {
      const result = scanLua(fs.readFileSync(file, 'utf8'));
      uses.push(...result.uses.map((use) => ({ ...use, file: relative })));
      unresolved.push(...result.unresolved.map((item) => ({ ...item, file: relative })));
    } else if (SCENE_EXTENSIONS.has(extension)) {
      let document;
      try {
        document = JSON.parse(fs.readFileSync(file, 'utf8'));
      } catch {
        unreadable.push(relative);
        continue;
      }
      const types = componentTypesIn(document);
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
  return { name: project.name, list, uses, unresolved, unreadable, needsJolt, needsPhysx };
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
  return list;
}

function main(argv) {
  const flags = new Set(argv.filter((arg) => arg.startsWith('--')));
  const folders = argv.filter((arg) => !arg.startsWith('--'));
  const known = new Set(['--suggest', '--json']);
  if (folders.length !== 1 || [...flags].some((flag) => !known.has(flag))) {
    console.error('usage: project-modules.mjs <project folder> [--suggest] [--json]');
    return 2;
  }
  let scan;
  try {
    scan = scanProject(folders[0]);
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
  for (const module of Object.keys(MODULES)) {
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
