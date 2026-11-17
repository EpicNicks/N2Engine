// The Lighting block (set 0, binding 1; 752 bytes; GpuShaderInterface.hpp kLitLighting and the light member tables).
// n2_EncodeOutput is the one member renamed (n2_EncodeOutputFlag): the helper function of that name encodes a colour.
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

layout(set = 0, binding = 1, std140) uniform Lighting {
    vec3 n2_CameraPos;
    int n2_EncodeOutputFlag;
    vec3 n2_AmbientLight;
    int n2_NumDirectionalLights;
    int n2_NumPointLights;
    int n2_NumSpotLights;
    N2DirectionalLight n2_DirectionalLights[2];
    N2PointLight n2_PointLights[8];
    N2SpotLight n2_SpotLights[4];
};
