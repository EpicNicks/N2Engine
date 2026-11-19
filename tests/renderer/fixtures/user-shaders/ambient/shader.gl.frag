#version 330 core
// n2shader contract 1, target gl
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


// Varyings of the engine's vertex stage (matched by name)
in vec2 n2_UV;
in vec4 n2_Color;
in vec3 n2_WorldPos;
in vec3 n2_Normal;
in vec4 n2_Tangent;

out vec4 n2_FragColor;


// The framebuffer position in pixels, origin top-left on every target
vec4 n2_FragCoord() {
    return vec4(gl_FragCoord.x, n2_TargetHeight - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w);
}

vec4 n2_texture(int slot, vec2 uv) {
    vec4 result = vec4(0.0);
    return result;
}

vec4 n2_textureLod(int slot, vec2 uv, float lod) {
    vec4 result = vec4(0.0);
    return result;
}

vec4 n2_texelFetch(int slot, ivec2 texel, int lod) {
    vec4 result = vec4(0.0);
    return result;
}

ivec2 n2_textureSize(int slot, int lod) {
    ivec2 result = ivec2(0);
    return result;
}

// Helper functions over the Lighting data, the same text on every target (the data's spelling differs, the names do not).
// The Blinn-Phong diffuse part of the lit shader (renderer/shaders/vulkan/lit.frag): ambient plus, per light, colour x
// intensity x N.L (x attenuation, x the spot cone). N is the unit surface normal, worldPos the fragment position.
float n2_Attenuation(float distance, float range, float attenuation) {
    float d = distance / range;
    return 1.0 / (1.0 + attenuation * d * d);
}

vec3 n2_LightDiffuse(vec3 N, vec3 worldPos) {
    vec3 lighting = n2_AmbientLight;
    for (int i = 0; i < n2_NumDirectionalLights; i++) {
        vec3 L = normalize(-n2_DirectionalLights[i].direction);
        lighting += n2_DirectionalLights[i].color * n2_DirectionalLights[i].intensity * max(dot(N, L), 0.0);
    }
    for (int i = 0; i < n2_NumPointLights; i++) {
        vec3 toFrag = worldPos - n2_PointLights[i].position;
        float dist = length(toFrag);
        if (dist > n2_PointLights[i].range) {
            continue;
        }
        vec3 L = normalize(-toFrag);
        lighting += n2_PointLights[i].color * n2_PointLights[i].intensity * max(dot(N, L), 0.0)
            * n2_Attenuation(dist, n2_PointLights[i].range, n2_PointLights[i].attenuation);
    }
    for (int i = 0; i < n2_NumSpotLights; i++) {
        vec3 toFrag = worldPos - n2_SpotLights[i].position;
        float dist = length(toFrag);
        if (dist > n2_SpotLights[i].range) {
            continue;
        }
        vec3 L = dist > 0.000001 ? -toFrag / dist : vec3(0.0);
        float theta = dot(L, -n2_SpotLights[i].direction);
        float cone = clamp((theta - n2_SpotLights[i].cosOuter) / max(n2_SpotLights[i].cosInner - n2_SpotLights[i].cosOuter, 0.0001), 0.0, 1.0);
        lighting += n2_SpotLights[i].color * n2_SpotLights[i].intensity * max(dot(N, L), 0.0)
            * n2_Attenuation(dist, n2_SpotLights[i].range, 1.0) * cone;
    }
    return lighting;
}

// The sRGB encode, for authors who light in linear space: the flag is 1 when the engine's linear lighting is on
vec4 n2_EncodeOutput(vec4 color) {
    if (n2_EncodeOutputFlag == 0) {
        return color;
    }
    vec3 c = max(color.rgb, vec3(0.0));
    return vec4(mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)), color.a);
}



// Generated from the header's params (contract 1); a bool is an int (0 or 1), a texture is its slot number for n2_texture
uniform float u_Scale;
#line 8 1
// Reads the scene lighting block: the ambient colour (set by UpdateSceneLighting) times u_Scale
void main() {
    n2_FragColor = vec4(n2_AmbientLight * u_Scale, 1.0);
}

