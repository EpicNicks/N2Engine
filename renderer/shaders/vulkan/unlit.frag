#version 450

// The Vulkan port of OpenGLRenderer.cpp's unlitFrag (GLSL 330): uAlbedo x texture x vertex colour, alpha-tested against
// uAlphaCutoff (0 keeps every fragment), then the colour effects (uSaturation, uFlash). The material block (set 1, binding 0) is one dynamic-offset range per draw;
// the base texture is set 1, binding 1.
layout(set = 1, binding = 0, std140) uniform Material {
    vec4 uAlbedo;
    int uHasTexture;
    float uAlphaCutoff;
    float uSaturation;
    vec4 uFlash;
};

layout(set = 1, binding = 1) uniform sampler2D uTexture;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragStageDim;
layout(location = 0) out vec4 FragColor;

// Common::ColorEffects: saturation (1 none, 0 grey), then the flash colour mixed over by its alpha (0 none)
vec3 colorEffected(vec3 rgb) {
    if (uSaturation != 1.0) {
        rgb = mix(vec3(dot(rgb, vec3(0.299, 0.587, 0.114))), rgb, uSaturation);
    }
    if (uFlash.a > 0.0) {
        rgb = mix(rgb, uFlash.rgb, min(uFlash.a, 1.0));
    }
    return rgb;
}

// RenderState::dim (RenderState::StageDimmed): the colour mixed towards a darkened grey of its own luminance
vec3 stageDimmed(vec3 rgb) {
    return mix(rgb, vec3(dot(rgb, vec3(0.299, 0.587, 0.114)) * 0.45), fragStageDim);
}

// The per-draw block (set 0, binding 0), declared as the vertex shader declares it: this stage reads the screen tint
layout(set = 0, binding = 0, std140, row_major) uniform PerDraw {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    float uStageDim;
    float uTintSaturation;
    int uTintInside;
    vec3 uTintCircle;
};

// RenderState::tint (ScreenTint): the saturation changed inside (uTintInside 1) or outside a circle on the screen.
// uTintCircle is the centre and radius in framebuffer pixels, row 0 at the top as gl_FragCoord counts here (the flipped
// viewport). Saturation 1 is no tint and costs one uniform branch
vec3 screenTinted(vec3 rgb) {
    if (uTintSaturation >= 1.0) {
        return rgb;
    }
    vec2 offset = gl_FragCoord.xy - uTintCircle.xy;
    bool inCircle = dot(offset, offset) <= uTintCircle.z * uTintCircle.z;
    if (inCircle != (uTintInside != 0)) {
        return rgb;
    }
    return mix(vec3(dot(rgb, vec3(0.299, 0.587, 0.114))), rgb, uTintSaturation);
}

void main() {
    vec4 color = uAlbedo;
    if (uHasTexture != 0) {
        color *= texture(uTexture, fragTexCoord);
    }
    color *= fragColor;
    if (color.a < uAlphaCutoff) {
        discard;
    }
    FragColor = vec4(screenTinted(stageDimmed(colorEffected(color.rgb))), color.a);
}
