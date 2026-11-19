struct Material {
  u_Color : vec4<f32>,
  u_Gain : f32,
  u_Offset : vec3<f32>,
  u_Flag : i32,
  u_Steps : i32,
  u_Pair : vec2<f32>,
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
  var rgb : vec3<f32>;
  rgb = ((v.u_Color.xyz * v.u_Gain) + v.u_Offset);
  let v_2 = (((f32((v.u_Steps - 2i)) * 0.0f) + ((v.u_Pair.x - 0.25f) * 0.0f)) + ((v.u_Pair.y - 0.5f) * 0.0f));
  rgb = (rgb + vec3<f32>(v_2, v_2, v_2));
  if ((v.u_Flag != 0i)) {
    rgb = (vec3<f32>(1.0f) - rgb);
  }
  let v_3 = rgb;
  n2_FragColor = vec4<f32>(v_3.x, v_3.y, v_3.z, v.u_Color.w);
}

@fragment
fn main(@location(0u) n2_UV : vec2<f32>, @location(1u) n2_Color : vec4<f32>, @location(2u) n2_WorldPos : vec3<f32>, @location(3u) n2_Normal : vec3<f32>, @location(4u) n2_Tangent : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent);
  return n2_FragColor;
}
