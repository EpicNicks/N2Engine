var<private> n2_FragColor : vec4<f32>;

struct Material {
  u_Color : vec4<f32>,
  u_Shift : f32,
}

@group(1u) @binding(0u) var<uniform> v : Material;

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
  var n2_sink : f32;
  n2_sink = ((((n2_UV.x + n2_Color.x) + n2_WorldPos.x) + n2_Normal.x) + n2_Tangent.x);
  v_2();
}

fn v_2() {
  n2_FragColor = v.u_Color;
}

@fragment
fn main(@location(0u) n2_UV : vec2<f32>, @location(1u) n2_Color : vec4<f32>, @location(2u) n2_WorldPos : vec3<f32>, @location(3u) n2_Normal : vec3<f32>, @location(4u) n2_Tangent : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent);
  return n2_FragColor;
}
