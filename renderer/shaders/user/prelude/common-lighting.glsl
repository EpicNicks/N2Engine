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
