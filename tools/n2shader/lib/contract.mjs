// Contract v1 of the user shaders (orchestrator/proposals/user-shaders.md, sections 2.1 and 3): the numbers every other
// module of the cook reads. The block layouts below are GpuShaderInterface.hpp's (renderer/include/renderer/common/),
// renamed with the n2_ prefix; n2shader.test.mjs parses that header and fails when the two drift.

export const FORMAT_VERSION = 1;
export const CONTRACT_VERSION = 1;

/** Hard limits, checked before any external compiler runs (proposal section 8) */
export const LIMITS = Object.freeze({
  sourceBytes: 256 * 1024,
  sourceLines: 20000,
  headerBytes: 64 * 1024,
  headerDepth: 8,
  params: 64,
  blockBytes: 256,
  textures: 5,
});

export const TEXTURE_BINDING_BASE = 1; // set 1, binding 1 + slot
export const SAMPLER_BINDING_BASE = 6; // set 1, binding 6 + slot (WebGPU only)

/** The set 0, binding 0 block: [name, glsl type, std140 offset] */
export const PER_DRAW = Object.freeze([
  ['n2_Model', 'mat4', 0],
  ['n2_View', 'mat4', 64],
  ['n2_Projection', 'mat4', 128],
  ['n2_StageDim', 'float', 192],
  ['n2_TintSaturation', 'float', 196],
  ['n2_TintInside', 'int', 200],
  ['n2_TintCircle', 'vec3', 208],
].map(([name, type, offset]) => Object.freeze({ name, type, offset })));

/** The set 0, binding 1 block (752 bytes), opt-in through "uses": ["lighting"] */
export const LIGHTING = Object.freeze([
  ['n2_CameraPos', 'vec3', 0],
  ['n2_EncodeOutputFlag', 'int', 12],
  ['n2_AmbientLight', 'vec3', 16],
  ['n2_NumDirectionalLights', 'int', 28],
  ['n2_NumPointLights', 'int', 32],
  ['n2_NumSpotLights', 'int', 36],
  ['n2_DirectionalLights', 'struct', 48],
  ['n2_PointLights', 'struct', 112],
  ['n2_SpotLights', 'struct', 496],
].map(([name, type, offset]) => Object.freeze({ name, type, offset })));
export const LIGHTING_BLOCK_SIZE = 752;

/** The engine vertex stage's varyings (fragment inputs) and the one colour output */
export const VARYINGS = Object.freeze([
  ['n2_UV', 0], ['n2_Color', 1], ['n2_WorldPos', 2], ['n2_Normal', 3], ['n2_Tangent', 4],
].map(([name, location]) => Object.freeze({ name, location })));
export const OUTPUT_NAME = 'n2_FragColor';

/** What `uses` may hold in v1 ("time" is not in the contract) */
export const USES = Object.freeze(['lighting']);

/** The attributes the engine's vertex stage supplies (fragment-only v1: the lit attribute set) */
export const VERTEX_ATTRIBUTES = Object.freeze(['aPos', 'aNormal', 'aTexCoord', 'aColor', 'aTangent']);

export const PARAM_TYPES = Object.freeze(['float', 'int', 'bool', 'vec2', 'vec3', 'vec4', 'color', 'texture']);
export const TEXTURE_DEFAULTS = Object.freeze(['white', 'black', 'normal', 'none']);
export const BLEND_MODES = Object.freeze(['none', 'alpha', 'additive']);
export const CULL_MODES = Object.freeze(['none', 'back']);
export const QUEUES = Object.freeze(['opaque', 'transparent']);

/** The reserved parameter names that bind to the standard Material fields */
export const RESERVED_PARAMS = Object.freeze({ baseColor: 'color', baseColorTexture: 'texture' });

/** `#extension` lines a user stage may carry (proposal section 8): none beyond this list */
export const ALLOWED_EXTENSIONS = Object.freeze(['GL_EXT_control_flow_attributes']);

/** SPIR-V capabilities a user shader's module may declare */
export const ALLOWED_CAPABILITIES = Object.freeze([
  'Shader', 'Matrix', 'ImageQuery', 'DerivativeControl', 'Sampled1D', 'SampledBuffer', 'Float16', 'Int16', 'Int8',
]);

export const TARGETS = Object.freeze(['vulkan', 'webgpu', 'gl']);
