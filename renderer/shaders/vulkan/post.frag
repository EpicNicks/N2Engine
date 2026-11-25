#version 450

// The post-processing passes (issue #445; VulkanPostProcess.cpp): the maths of Common::ApplyPostProcessCpu, the OpenGL
// renderer's inline GLSL and renderer/shaders/webgpu/post.wgsl. p.a = (mode, threshold, intensity, invert),
// p.b = (stepX, stepY, chromatic aberration, vignette), p.c = (tint rgb, flipY: unused here, captures are blits).

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 0, binding = 1) uniform sampler2D uBloom;

layout(push_constant) uniform PostParams
{
    vec4 a;
    vec4 b;
    vec4 c;
} p;

void main()
{
    int mode = int(p.a.x + 0.5);
    vec2 uv = vUv;
    if (mode == 0)
    {
        outColor = vec4(texture(uSource, uv).rgb, 1.0);
    }
    else if (mode == 1)
    {
        vec3 c = texture(uSource, uv).rgb;
        float brightest = max(c.r, max(c.g, c.b));
        float weight = max(brightest - p.a.y, 0.0) / max(brightest, 1e-4);
        outColor = vec4(c * weight, 1.0);
    }
    else if (mode == 2)
    {
        vec2 o1 = p.b.xy;
        vec2 o2 = p.b.xy * 2.0;
        vec2 o3 = p.b.xy * 3.0;
        vec2 o4 = p.b.xy * 4.0;
        vec3 sum = texture(uSource, uv).rgb * 0.2270270270;
        sum += (texture(uSource, uv + o1).rgb + texture(uSource, uv - o1).rgb) * 0.1945945946;
        sum += (texture(uSource, uv + o2).rgb + texture(uSource, uv - o2).rgb) * 0.1216216216;
        sum += (texture(uSource, uv + o3).rgb + texture(uSource, uv - o3).rgb) * 0.0540540541;
        sum += (texture(uSource, uv + o4).rgb + texture(uSource, uv - o4).rgb) * 0.0162162162;
        outColor = vec4(sum, 1.0);
    }
    else if (mode == 4)
    {
        vec4 base = texture(uSource, uv);
        outColor = vec4(clamp(mix(base.rgb, texture(uBloom, uv).rgb, p.a.z), 0.0, 1.0), base.a);
    }
    else if (mode == 5)
    {
        bool topFirst = p.b.y > 0.5;
        vec2 point = vec2(uv.x, topFirst ? 1.0 - uv.y : uv.y);
        vec2 fromCentre = point - 0.5;
        float radius = dot(fromCentre, fromCentre) * 2.0;
        vec2 source = 0.5 + fromCentre * (1.0 + p.a.y * 0.5 * (radius - 1.0));
        if (p.a.z > 0.0)
        {
            source += p.a.z * sin(6.28318530718 * (p.a.w * point.yx + p.b.x));
        }
        if (topFirst)
        {
            source.y = 1.0 - source.y;
        }
        outColor = vec4(texture(uSource, source).rgb, texture(uSource, uv).a);
    }
    else
    {
        vec2 d = (uv - 0.5) * p.b.z;
        vec4 base = texture(uSource, uv);
        vec3 rgb = vec3(texture(uSource, uv + d).r, base.g, texture(uSource, uv - d).b);
        rgb += texture(uBloom, uv).rgb * p.a.z * p.c.rgb;
        rgb = clamp(rgb, 0.0, 1.0);
        rgb = mix(rgb, 1.0 - rgb, p.a.w);
        float distance = length(uv - 0.5) * 1.41421356;
        rgb = clamp(rgb * (1.0 - p.b.w * smoothstep(0.3, 1.0, distance)), 0.0, 1.0);
        outColor = vec4(rgb, base.a);
    }
}
