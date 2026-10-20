// Must be REJECTED by Tint and naga (scripts/validate-wgsl.sh checks it): a type error.
@fragment
fn fs_main() -> @location(0) vec4<f32> {
    let wrong: f32 = vec3<f32>(1.0, 2.0, 3.0);
    return vec4<f32>(wrong);
}
