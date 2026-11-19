var<private> n2_FragColor : vec4<f32>;

struct N2DirectionalLight {
  direction : vec3<f32>,
  color : vec3<f32>,
  intensity : f32,
}

struct N2PointLight {
  position : vec3<f32>,
  color : vec3<f32>,
  intensity : f32,
  range : f32,
  attenuation : f32,
}

struct N2SpotLight {
  position : vec3<f32>,
  direction : vec3<f32>,
  color : vec3<f32>,
  intensity : f32,
  range : f32,
  cosInner : f32,
  cosOuter : f32,
}

struct Lighting {
  n2_CameraPos : vec3<f32>,
  n2_EncodeOutputFlag : i32,
  n2_AmbientLight : vec3<f32>,
  n2_NumDirectionalLights : i32,
  n2_NumPointLights : i32,
  n2_NumSpotLights : i32,
  n2_DirectionalLights : array<N2DirectionalLight, 2u>,
  n2_PointLights : array<N2PointLight, 8u>,
  n2_SpotLights : array<N2SpotLight, 4u>,
}

@group(0u) @binding(1u) var<uniform> v : Lighting;

struct Material {
  u_Scale : f32,
}

@group(1u) @binding(0u) var<uniform> v_1 : Material;

struct PerDraw_1 {
  n2_Model : mat4x4<f32>,
  n2_View : mat4x4<f32>,
  n2_Projection : mat4x4<f32>,
  n2_StageDim : f32,
  n2_TintSaturation : f32,
  n2_TintInside : i32,
  n2_TintCircle : vec3<f32>,
}

@group(0u) @binding(0u) var<uniform> v_2 : PerDraw_1;

fn main_inner(n2_UV : vec2<f32>, n2_Color : vec4<f32>, n2_WorldPos : vec3<f32>, n2_Normal : vec3<f32>, n2_Tangent : vec4<f32>) {
  let v_3 = (v.n2_AmbientLight * v_1.u_Scale);
  n2_FragColor = vec4<f32>(v_3.x, v_3.y, v_3.z, 1.0f);
}

@fragment
fn main(@location(0u) n2_UV : vec2<f32>, @location(1u) n2_Color : vec4<f32>, @location(2u) n2_WorldPos : vec3<f32>, @location(3u) n2_Normal : vec3<f32>, @location(4u) n2_Tangent : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent);
  return n2_FragColor;
}
