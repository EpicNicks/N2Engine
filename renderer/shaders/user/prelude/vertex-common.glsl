// The engine's vertex maths, shared by the three targets' custom vertex stages (stage 9d). Included by vertex-vulkan.glsl,
// vertex-webgpu.glsl and vertex-opengl.glsl after they declare the attributes (n2_in_*), the varyings (n2_UV ... n2_Tangent as outputs),
// the PerDraw uniforms and N2_CLIP_Z.

bool n2_positionSet = false;

// Writes the clip-space position, with the target's depth remap applied (the author gives OpenGL clip space, z from -w to w)
void n2_SetPosition(vec4 clip) {
    clip.z = N2_CLIP_Z(clip);
    gl_Position = clip;
    n2_positionSet = true;
}

// What the built-in vertex stage does: world position, normal matrix, unit tangent with the mirror sign, uv and colour. The generated
// main calls it first, then the author's n2_vertex(), which may change any of the five varyings
void n2_DefaultVertex() {
    vec4 worldPos = n2_Model * vec4(n2_in_Position, 1.0);
    n2_WorldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(n2_Model)));
    n2_Normal = normalize(normalMatrix * n2_in_Normal);
    vec3 worldTangent = mat3(n2_Model) * n2_in_Tangent.xyz;
    float worldTangentLength = length(worldTangent);
    worldTangent = worldTangentLength > 0.0 ? worldTangent / worldTangentLength : vec3(0.0);
    n2_Tangent = vec4(worldTangent, n2_in_Tangent.w * (determinant(mat3(n2_Model)) < 0.0 ? -1.0 : 1.0));
    n2_UV = n2_in_UV;
    n2_Color = n2_in_Color;
}
