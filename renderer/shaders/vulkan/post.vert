#version 450

// The post-processing passes' vertex stage (issue #445; VulkanPostProcess.cpp): one full-screen triangle from gl_VertexIndex, no
// vertex buffer. The passes keep the frame's flipped viewport (a negative height: NDC y +1 is the image's first row), and a
// texture's first row is v = 0, so v = (1 - y) / 2 reads the source the right way up.

layout(location = 0) out vec2 vUv;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
    vUv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
}
