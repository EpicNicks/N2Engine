var<private> n2_positionSet : bool;

struct gl_PerVertex {
  gl_Position : vec4<f32>,
  gl_PointSize : f32,
  gl_ClipDistance : array<f32, 1u>,
  gl_CullDistance : array<f32, 1u>,
}

var<private> v : gl_PerVertex;

struct PerDraw_1 {
  n2_Model : mat4x4<f32>,
  n2_View : mat4x4<f32>,
  n2_Projection : mat4x4<f32>,
  n2_StageDim : f32,
  n2_TintSaturation : f32,
  n2_TintInside : i32,
  n2_TintCircle : vec3<f32>,
}

@group(0u) @binding(0u) var<uniform> v_1 : PerDraw_1;

var<private> n2_WorldPos : vec3<f32>;

var<private> n2_Normal : vec3<f32>;

var<private> n2_Tangent : vec4<f32>;

var<private> n2_UV : vec2<f32>;

var<private> n2_Color : vec4<f32>;

struct Material {
  u_Color : vec4<f32>,
  u_Shift : f32,
}

@group(1u) @binding(0u) var<uniform> v_2 : Material;

fn main_inner(n2_in_Position : vec3<f32>, n2_in_Normal : vec3<f32>, n2_in_Tangent : vec4<f32>, n2_in_UV : vec2<f32>, n2_in_Color : vec4<f32>) {
  var param : vec4<f32>;
  n2_positionSet = false;
  v_3(n2_in_Position, n2_in_Normal, n2_in_Tangent, n2_in_UV, n2_in_Color);
  v_4();
  if (!(n2_positionSet)) {
    let v_5 = (v_1.n2_Projection * v_1.n2_View);
    let v_6 = n2_WorldPos;
    param = (v_5 * vec4<f32>(v_6.x, v_6.y, v_6.z, 1.0f));
    v_7(&(param));
  }
}

fn v_7(clip : ptr<function, vec4<f32>>) {
  (*(clip)).z = (((*(clip)).z + (*(clip)).w) * 0.5f);
  v.gl_Position = *(clip);
  n2_positionSet = true;
}

fn v_3(n2_in_Position : vec3<f32>, n2_in_Normal : vec3<f32>, n2_in_Tangent : vec4<f32>, n2_in_UV : vec2<f32>, n2_in_Color : vec4<f32>) {
  var worldPos : vec4<f32>;
  var normalMatrix : mat3x3<f32>;
  var worldTangent : vec3<f32>;
  var worldTangentLength : f32;
  var v_8 : vec3<f32>;
  worldPos = (v_1.n2_Model * vec4<f32>(n2_in_Position.x, n2_in_Position.y, n2_in_Position.z, 1.0f));
  n2_WorldPos = worldPos.xyz;
  let v_9 = v_1.n2_Model;
  let v_10 = mat3x3<f32>(v_9[0u].xyz, v_9[1u].xyz, v_9[2u].xyz);
  let v_11 = v_10[0u].x;
  let v_12 = v_10[0u].y;
  let v_13 = v_10[0u].z;
  let v_14 = v_10[1u].x;
  let v_15 = v_10[1u].y;
  let v_16 = v_10[1u].z;
  let v_17 = v_10[2u].x;
  let v_18 = v_10[2u].y;
  let v_19 = v_10[2u].z;
  normalMatrix = transpose(((1.0f / determinant(v_10)) * mat3x3<f32>(vec3<f32>(((v_15 * v_19) - (v_16 * v_18)), ((v_13 * v_18) - (v_12 * v_19)), ((v_12 * v_16) - (v_13 * v_15))), vec3<f32>(((v_16 * v_17) - (v_14 * v_19)), ((v_11 * v_19) - (v_13 * v_17)), ((v_13 * v_14) - (v_11 * v_16))), vec3<f32>(((v_14 * v_18) - (v_15 * v_17)), ((v_12 * v_17) - (v_11 * v_18)), ((v_11 * v_15) - (v_12 * v_14))))));
  n2_Normal = normalize((normalMatrix * n2_in_Normal));
  let v_20 = v_1.n2_Model;
  worldTangent = (mat3x3<f32>(v_20[0u].xyz, v_20[1u].xyz, v_20[2u].xyz) * n2_in_Tangent.xyz);
  worldTangentLength = length(worldTangent);
  if ((worldTangentLength > 0.0f)) {
    let v_21 = worldTangent;
    let v_22 = worldTangentLength;
    v_8 = (v_21 / vec3<f32>(v_22, v_22, v_22));
  } else {
    v_8 = vec3<f32>();
  }
  worldTangent = v_8;
  let v_23 = worldTangent;
  let v_24 = v_1.n2_Model;
  n2_Tangent = vec4<f32>(v_23.x, v_23.y, v_23.z, (n2_in_Tangent.w * select(1.0f, -1.0f, (determinant(mat3x3<f32>(v_24[0u].xyz, v_24[1u].xyz, v_24[2u].xyz)) < 0.0f))));
  n2_UV = n2_in_UV;
  n2_Color = n2_in_Color;
}

fn v_4() {
  let v_25 = v_2.u_Shift;
  n2_WorldPos.x = (n2_WorldPos.x + v_25);
}

struct tint_symbol {
  @builtin(position)
  gl_Position : vec4<f32>,
  @location(2u)
  n2_WorldPos : vec3<f32>,
  @location(3u)
  n2_Normal : vec3<f32>,
  @location(4u)
  n2_Tangent : vec4<f32>,
  @location(0u)
  n2_UV : vec2<f32>,
  @location(1u)
  n2_Color : vec4<f32>,
}

@vertex
fn main(@location(0u) n2_in_Position : vec3<f32>, @location(1u) n2_in_Normal : vec3<f32>, @location(4u) n2_in_Tangent : vec4<f32>, @location(2u) n2_in_UV : vec2<f32>, @location(3u) n2_in_Color : vec4<f32>) -> tint_symbol {
  main_inner(n2_in_Position, n2_in_Normal, n2_in_Tangent, n2_in_UV, n2_in_Color);
  return tint_symbol(v.gl_Position, n2_WorldPos, n2_Normal, n2_Tangent, n2_UV, n2_Color);
}
