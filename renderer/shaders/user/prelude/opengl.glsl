// Contract v1 prelude, OpenGL target (GLSL 330 core text, compiled by the driver). Not valid GLSL on its own: a template,
// see README.md. Every uniform is a plain named uniform the renderer sets by name, as it does for the built-in shaders.

uniform mat4 n2_Model;
uniform mat4 n2_View;
uniform mat4 n2_Projection;
uniform float n2_StageDim;
uniform float n2_TintSaturation;
uniform int n2_TintInside;
uniform vec3 n2_TintCircle;
// The render target's height in pixels (OpenGL's window origin is bottom-left; n2_FragCoord() flips it)
uniform float n2_TargetHeight;

//@if-lighting
struct N2DirectionalLight {
    vec3 direction;
    vec3 color;
    float intensity;
};

struct N2PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float range;
    float attenuation;
};

struct N2SpotLight {
    vec3 position;
    vec3 direction;
    vec3 color;
    float intensity;
    float range;
    float cosInner;
    float cosOuter;
};

uniform vec3 n2_CameraPos;
uniform int n2_EncodeOutputFlag;
uniform vec3 n2_AmbientLight;
uniform int n2_NumDirectionalLights;
uniform int n2_NumPointLights;
uniform int n2_NumSpotLights;
uniform N2DirectionalLight n2_DirectionalLights[2];
uniform N2PointLight n2_PointLights[8];
uniform N2SpotLight n2_SpotLights[4];
//@end-if

// Varyings of the engine's vertex stage (matched by name)
in vec2 n2_UV;
in vec4 n2_Color;
in vec3 n2_WorldPos;
in vec3 n2_Normal;
in vec4 n2_Tangent;

out vec4 n2_FragColor;

//@each-texture
uniform sampler2D n2_tex@SLOT@;
//@end-each

// The framebuffer position in pixels, origin top-left on every target
vec4 n2_FragCoord() {
    return vec4(gl_FragCoord.x, n2_TargetHeight - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w);
}

vec4 n2_texture(int slot, vec2 uv) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = texture(n2_tex@SLOT@, uv); }
//@end-each
    return result;
}

vec4 n2_textureLod(int slot, vec2 uv, float lod) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = textureLod(n2_tex@SLOT@, uv, lod); }
//@end-each
    return result;
}

vec4 n2_texelFetch(int slot, ivec2 texel, int lod) {
    vec4 result = vec4(0.0);
//@each-texture
    if (slot == @SLOT@) { result = texelFetch(n2_tex@SLOT@, texel, lod); }
//@end-each
    return result;
}

ivec2 n2_textureSize(int slot, int lod) {
    ivec2 result = ivec2(0);
//@each-texture
    if (slot == @SLOT@) { result = textureSize(n2_tex@SLOT@, lod); }
//@end-each
    return result;
}

//@if-lighting
//@include common-lighting.glsl
//@end-if
