@group(1u) @binding(1u) var n2_tex0 : texture_2d<f32>;

@group(1u) @binding(6u) var n2_samp0 : sampler;

@group(1u) @binding(2u) var n2_tex1 : texture_2d<f32>;

@group(1u) @binding(7u) var n2_samp1 : sampler;

struct Material {
  u_Dir : vec3<f32>,
  u_W : f32,
  u_Mode : i32,
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
  var color : vec4<f32>;
  var param : i32;
  var param_1 : vec3<f32>;
  var param_2 : i32;
  var param_3 : i32;
  var param_4 : vec3<f32>;
  if ((v.u_Mode == 0i)) {
    let v_2 = normalize(v.u_Dir);
    param = 0i;
    param_1 = v_2;
    color = v_3(&(param), &(param_1));
  } else {
    let v_4 = vec3<f32>(0.5f, 0.5f, v.u_W);
    param_2 = 1i;
    param_3 = 2i;
    param_4 = v_4;
    color = v_5(&(param_2), &(param_3), &(param_4));
  }
  let v_6 = color.xyz;
  n2_FragColor = vec4<f32>(v_6.x, v_6.y, v_6.z, 1.0f);
}

fn v_7(slot : ptr<function, i32>, uv : ptr<function, vec2<f32>>, lod : ptr<function, f32>) -> vec4<f32> {
  var result : vec4<f32>;
  result = vec4<f32>();
  if ((*(slot) == 0i)) {
    result = textureSampleLevel(n2_tex0, n2_samp0, *(uv), *(lod));
  }
  if ((*(slot) == 1i)) {
    result = textureSampleLevel(n2_tex1, n2_samp1, *(uv), *(lod));
  }
  return result;
}

fn v_8(slot : ptr<function, i32>, lod : ptr<function, i32>) -> vec2<i32> {
  var result : vec2<i32>;
  result = vec2<i32>();
  if ((*(slot) == 0i)) {
    result = vec2<i32>(textureDimensions(n2_tex0, *(lod)));
  }
  if ((*(slot) == 1i)) {
    result = vec2<i32>(textureDimensions(n2_tex1, *(lod)));
  }
  return result;
}

fn v_3(slot : ptr<function, i32>, dir : ptr<function, vec3<f32>>) -> vec4<f32> {
  var a : vec3<f32>;
  var ma : f32;
  var face : f32;
  var sc_tc : vec2<f32>;
  var v_9 : vec2<f32>;
  var v_10 : vec2<f32>;
  var v_11 : vec2<f32>;
  var st : vec2<f32>;
  var size : f32;
  var param : i32;
  var param_5 : i32;
  var inset : f32;
  var param_6 : i32;
  var param_7 : vec2<f32>;
  var param_8 : f32;
  a = abs(*(dir));
  ma = max(max(a.x, a.y), a.z);
  if ((ma <= 0.0f)) {
    return vec4<f32>();
  }
  let v_12 = (a.x >= a.y);
  var v_13 : bool;
  if (v_12) {
    v_13 = (a.x >= a.z);
  } else {
    v_13 = v_12;
  }
  if (v_13) {
    face = select(1.0f, 0.0f, ((*(dir)).x > 0.0f));
    if (((*(dir)).x > 0.0f)) {
      v_9 = vec2<f32>(-((*(dir)).z), -((*(dir)).y));
    } else {
      v_9 = vec2<f32>((*(dir)).z, -((*(dir)).y));
    }
    sc_tc = v_9;
  } else if ((a.y >= a.z)) {
    face = select(3.0f, 2.0f, ((*(dir)).y > 0.0f));
    if (((*(dir)).y > 0.0f)) {
      v_10 = vec2<f32>((*(dir)).x, (*(dir)).z);
    } else {
      v_10 = vec2<f32>((*(dir)).x, -((*(dir)).z));
    }
    sc_tc = v_10;
  } else {
    face = select(5.0f, 4.0f, ((*(dir)).z > 0.0f));
    if (((*(dir)).z > 0.0f)) {
      v_11 = vec2<f32>((*(dir)).x, -((*(dir)).y));
    } else {
      v_11 = vec2<f32>(-((*(dir)).x), -((*(dir)).y));
    }
    sc_tc = v_11;
  }
  let v_14 = sc_tc;
  let v_15 = ma;
  st = (((v_14 / vec2<f32>(v_15, v_15)) * 0.5f) + vec2<f32>(0.5f));
  param = *(slot);
  param_5 = 0i;
  size = max(f32(v_8(&(param), &(param_5)).y), 1.0f);
  inset = (0.5f / size);
  let v_16 = st;
  let v_17 = inset;
  let v_18 = (1.0f - inset);
  st = clamp(v_16, vec2<f32>(v_17, v_17), vec2<f32>(v_18, v_18));
  let v_19 = vec2<f32>(((face + st.x) / 6.0f), st.y);
  param_6 = *(slot);
  param_7 = v_19;
  param_8 = 0.0f;
  return v_7(&(param_6), &(param_7), &(param_8));
}

fn v_5(slot : ptr<function, i32>, depth : ptr<function, i32>, uvw : ptr<function, vec3<f32>>) -> vec4<f32> {
  var d : f32;
  var w : f32;
  var i0 : f32;
  var i1 : f32;
  var f : f32;
  var size : vec2<f32>;
  var param : i32;
  var param_9 : i32;
  var inset : vec2<f32>;
  var xy : vec2<f32>;
  var a : vec4<f32>;
  var param_10 : i32;
  var param_11 : vec2<f32>;
  var param_12 : f32;
  var b : vec4<f32>;
  var param_13 : i32;
  var param_14 : vec2<f32>;
  var param_15 : f32;
  d = f32(max(*(depth), 1i));
  w = ((clamp((*(uvw)).z, 0.0f, 1.0f) * d) - 0.5f);
  i0 = clamp(floor(w), 0.0f, (d - 1.0f));
  i1 = min((i0 + 1.0f), (d - 1.0f));
  f = clamp((w - i0), 0.0f, 1.0f);
  param = *(slot);
  param_9 = 0i;
  size = max(vec2<f32>(v_8(&(param), &(param_9))), vec2<f32>(1.0f));
  inset = vec2<f32>((0.5f / max((size.x / d), 1.0f)), (0.5f / size.y));
  xy = clamp((*(uvw)).xy, inset, (vec2<f32>(1.0f) - inset));
  let v_20 = vec2<f32>(((i0 + xy.x) / d), xy.y);
  param_10 = *(slot);
  param_11 = v_20;
  param_12 = 0.0f;
  a = v_7(&(param_10), &(param_11), &(param_12));
  let v_21 = vec2<f32>(((i1 + xy.x) / d), xy.y);
  param_13 = *(slot);
  param_14 = v_21;
  param_15 = 0.0f;
  b = v_7(&(param_13), &(param_14), &(param_15));
  let v_22 = a;
  let v_23 = b;
  let v_24 = f;
  return mix(v_22, v_23, vec4<f32>(v_24, v_24, v_24, v_24));
}

@fragment
fn main(@location(0u) n2_UV : vec2<f32>, @location(1u) n2_Color : vec4<f32>, @location(2u) n2_WorldPos : vec3<f32>, @location(3u) n2_Normal : vec3<f32>, @location(4u) n2_Tangent : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(n2_UV, n2_Color, n2_WorldPos, n2_Normal, n2_Tangent);
  return n2_FragColor;
}
