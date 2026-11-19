@group(1u) @binding(1u) var n2_tex0 : texture_2d<f32>;

@group(1u) @binding(6u) var n2_samp0 : sampler;

@group(1u) @binding(2u) var n2_tex1 : texture_2d<f32>;

@group(1u) @binding(7u) var n2_samp1 : sampler;

@group(1u) @binding(3u) var n2_tex2 : texture_2d<f32>;

@group(1u) @binding(8u) var n2_samp2 : sampler;

struct Material {
  u_Tint : vec4<f32>,
}

@group(1u) @binding(0u) var<uniform> v : Material;

var<private> n2_FragColor : vec4<f32>;

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

fn main_inner(n2_UV : vec2<f32>, n2_Color : vec4<f32>, n2_WorldPos : vec3<f32>, n2_Normal : vec3<f32>, n2_Tangent : vec4<f32>) {
  var base : vec4<f32>;
  var param : i32;
  var param_1 : vec2<f32>;
  var add : vec3<f32>;
  var param_2 : i32;
  var param_3 : vec2<f32>;
  var flatNormal : f32;
  var param_4 : i32;
  var param_5 : vec2<f32>;
  param = 0i;
  param_1 = n2_UV;
  base = (v_2(&(param), &(param_1)) * v.u_Tint);
  param_2 = 1i;
  param_3 = n2_UV;
  add = v_2(&(param_2), &(param_3)).xyz;
  param_4 = 2i;
  param_5 = n2_UV;
  flatNormal = v_2(&(param_4), &(param_5)).x;
  let v_3 = (base.xyz + add);
  n2_FragColor = vec4<f32>(v_3.x, v_3.y, v_3.z, flatNormal);
}

fn v_2(slot : ptr<function, i32>, uv : ptr<function, vec2<f32>>) -> vec4<f32> {
  var result : vec4<f32>;
  result = vec4<f32>();
  if ((*(slot) == 0i)) {
    result = textureSample(n2_tex0, n2_samp0, *(uv));
  }
  if ((*(slot) == 1i)) {
    result = textureSample(n2_tex1, n2_samp1, *(uv));
  }
  if ((*(slot) == 2i)) {
    result = textureSample(n2_tex2, n2_samp2, *(uv));
  }
  return result;
}

@fragment
fn main(@location(0u) n2_UV : vec2<f32>, @location(1u) n2_Color : vec4<f32>, @location(2u) n2_WorldPos : vec3<f32>, @location(3u) n2_Normal : vec3<f32>, @location(4u) n2_Tangent : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent);
  return n2_FragColor;
}
