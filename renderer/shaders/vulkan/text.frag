#version 450

// The Vulkan port of OpenGLRenderer.cpp's textFrag (GLSL 330): the antialiased SDF text shader. uTexture is a
// single-channel SDF atlas: the glyph edge is at 0.5 (128 in the atlas bytes), inside is higher. The coverage ramps from 0
// to 1 across about one screen pixel around the edge, measured with fwidth, and the atlas is read through .r. Effects
// (outline, shadow, softness) are material values that are all 0 when off. Compiled and pipeline-checked in stage B of
// #43 and drawn since stage E.
layout(set = 1, binding = 0, std140) uniform Material {
    vec4 uAlbedo;
    // Effects (TextDrawing::EffectUniforms), all 0 when off: distances are in atlas value units
    float uOutline;         // the outline's outer edge is at 0.5 - uOutline
    vec4 uOutlineColor;
    float uSoftness;        // the edge ramp's half width, at least the antialiasing ramp's
    vec4 uShadowColor;      // alpha 0: no shadow
    vec2 uShadowOffset;     // in atlas uv: the shadow at uv is the shape at uv - uShadowOffset
    float uShadowSoftness;
};

layout(set = 1, binding = 1) uniform sampler2D uTexture;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 FragColor;

// a over b, neither premultiplied
vec4 over(vec4 a, vec4 b) {
    float alpha = a.a + b.a * (1.0 - a.a);
    if (alpha <= 0.0) {
        return vec4(0.0);
    }
    return vec4((a.rgb * a.a + b.rgb * b.a * (1.0 - a.a)) / alpha, alpha);
}

void main() {
    float sdf = texture(uTexture, fragTexCoord).r;
    // Half the change over one pixel each side: a ramp one pixel wide (or wider, with softness)
    float edgeWidth = max(max(0.5 * fwidth(sdf), 1e-4), uSoftness);
    float coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf);

    vec4 color = uAlbedo * fragColor;
    float alpha = color.a * coverage;

    // Without effects this is the whole shader: the face colour, alpha-scaled by coverage
    vec4 result = vec4(color.rgb, alpha);
    if (uOutline > 0.0) {
        // The outline fills from its outer edge inwards, under the face
        float outlineEdge = 0.5 - uOutline;
        // The ramp's lower end never goes below 0, the distance at the quad's border: minified text
        // has a wide antialiasing ramp, which would otherwise give the whole quad some coverage
        float outlineCoverage = smoothstep(max(outlineEdge - edgeWidth, 0.0), outlineEdge + edgeWidth, sdf);
        result = over(result, vec4(uOutlineColor.rgb, uOutlineColor.a * outlineCoverage));
    }
    if (uShadowColor.a > 0.0) {
        // The shadow is the face-plus-outline shape, moved, under both
        float shadowSdf = texture(uTexture, fragTexCoord - uShadowOffset).r;
        float shadowWidth = max(max(0.5 * fwidth(shadowSdf), 1e-4), uShadowSoftness);
        float shadowEdge = 0.5 - uOutline;
        float shadowCoverage = smoothstep(max(shadowEdge - shadowWidth, 0.0), shadowEdge + shadowWidth, shadowSdf);
        result = over(result, vec4(uShadowColor.rgb, uShadowColor.a * shadowCoverage));
    }

    if (result.a < 0.01) {
        discard;
    }
    FragColor = result;
}
