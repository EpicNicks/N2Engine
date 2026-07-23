#include <cstring>
#include <cmath>
#include <algorithm>
#include <cassert>

#include "renderer/software/SWMaterial.hpp"
#include "renderer/software/SWMesh.hpp"
#include "renderer/software/SWShader.hpp"
#include "renderer/software/SWTexture.hpp"
#include "renderer/software/SoftwareRenderer.hpp"

using namespace Renderer;
using namespace Renderer::Common;
using namespace Renderer::Software;

bool SoftwareRenderer::Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height)
{
    m_window = windowHandle;
    glfwMakeContextCurrent(windowHandle);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return false;

    m_unlitShader = std::make_unique<SWShader>(SWShaderType::Unlit);
    m_litShader = std::make_unique<SWShader>(SWShaderType::Lit);

    Resize(width, height);
    if (!SetupBlitResources()) return false;

    m_renderThread.Start();
    return true;
}

void SoftwareRenderer::Shutdown()
{
    // Stop thread BEFORE tearing down GL/resources it might reference.
    m_renderThread.Stop();

    m_meshes.clear();
    m_textures.clear();
    m_materials.clear();
    m_shaders.clear();

    glDeleteTextures(1, &m_blitTex);
    glDeleteVertexArrays(1, &m_blitVAO);
    glDeleteBuffers(1, &m_blitVBO);
    glDeleteProgram(m_blitProg);
}

void SoftwareRenderer::Resize(uint32_t w, uint32_t h)
{
    m_width = w;
    m_height = h;
    m_colorBuffer.assign(w * h, 0xFF000000);
    m_depthBuffer.assign(w * h, 1.0f);
    if (m_blitTex)
    {
        glBindTexture(GL_TEXTURE_2D, m_blitTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
}

void SoftwareRenderer::OnResize(int w, int h) { Resize((uint32_t)w, (uint32_t)h); }

void SoftwareRenderer::Clear(float r, float g, float b, float a)
{
    m_clearR = r;
    m_clearG = g;
    m_clearB = b;
    m_clearA = a;
}

void SoftwareRenderer::BeginFrame()
{
    m_drawQueue.clear();
}

void SoftwareRenderer::EndFrame()
{
    // Capture state needed by render thread
    auto queue    = m_drawQueue;
    auto view     = std::vector<float>(m_view, m_view + 16);
    auto proj     = std::vector<float>(m_proj, m_proj + 16);
    auto lighting = m_lighting;

    m_renderThread.SubmitFrame({
        [this, queue = std::move(queue), view = std::move(view),
         proj = std::move(proj), lighting = std::move(lighting)]() mutable
        {
            ClearBuffers();

            memcpy(m_view, view.data(), 64);
            memcpy(m_proj, proj.data(), 64);
            m_lighting = lighting;

            for (auto& cmd : queue)
                RasterizeMesh(cmd.mesh, cmd.modelMatrix, cmd.material);

            // NOTE: GL upload moved to Present() — GL context lives on the main thread.
        }
    });
}

void SoftwareRenderer::Present()
{
    // Wait for the render thread to finish rasterizing into m_colorBuffer
    m_renderThread.WaitForFrame();

    // Upload CPU framebuffer to GL texture (main thread — context is current here)
    glBindTexture(GL_TEXTURE_2D, m_blitTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (GLsizei)m_width, (GLsizei)m_height,
                    GL_RGBA, GL_UNSIGNED_BYTE, m_colorBuffer.data());

    // Clear the GL backbuffer before blitting (fullscreen quad covers it, but safer)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Draw fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_blitProg);
    glBindVertexArray(m_blitVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_blitTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    // Swap buffers — without this nothing is ever visible
    if (m_window) glfwSwapBuffers(m_window);
}

IShader* SoftwareRenderer::CreateShaderProgram(const char *, const char *)
{
    auto shader = std::make_unique<SWShader>(SWShaderType::Unlit);
    IShader *raw = shader.get();
    m_shaders.push_back(std::move(shader));
    return raw;
}

void SoftwareRenderer::UseShaderProgram(IShader *) {} // no-op; shader chosen per-draw via material

bool SoftwareRenderer::DestroyShaderProgram(IShader *shader)
{
    if (shader == m_unlitShader.get() || shader == m_litShader.get())
    {
        return true;
    }

    const auto it = std::ranges::find_if(m_shaders, [shader](const auto &p) { return p.get() == shader; });
    if (it == m_shaders.end())
    {
        return false;
    }
    m_shaders.erase(it);
    return true;
}

bool SoftwareRenderer::IsValidShader(IShader *shader) const
{
    return shader && shader->IsValid();
}

IMesh* SoftwareRenderer::CreateMesh(const MeshData &d)
{
    auto mesh = std::make_unique<SWMesh>();
    mesh->vertices = d.vertices;
    mesh->indices = d.indices;
    IMesh *raw = mesh.get();
    m_meshes.push_back(std::move(mesh));
    return raw;
}

void SoftwareRenderer::DestroyMesh(IMesh *mesh)
{
    if (const auto it = std::ranges::find_if(m_meshes, [mesh](const auto &p) { return p.get() == mesh; });
        it != m_meshes.end())
    {
        m_meshes.erase(it);
    }
}

ITexture* SoftwareRenderer::CreateTexture(const uint8_t *data, uint32_t w, uint32_t h, uint32_t ch)
{
    auto tex = std::make_unique<SWTexture>();
    tex->width = w;
    tex->height = h;
    tex->channels = ch;
    tex->data.assign(data, data + w * h * ch);
    ITexture *raw = tex.get();
    m_textures.push_back(std::move(tex));
    return raw;
}

void SoftwareRenderer::DestroyTexture(ITexture *texture)
{
    if (const auto it = std::ranges::find_if(m_textures, [texture](const auto &p) { return p.get() == texture; });
        it != m_textures.end())
    {
        m_textures.erase(it);
    }
}

IMaterial* SoftwareRenderer::CreateMaterial(IShader *shader)
{
    auto mat = std::make_unique<SWMaterial>(shader);
    IMaterial *raw = mat.get();
    m_materials.push_back(std::move(mat));
    return raw;
}

IMaterial* SoftwareRenderer::CreateMaterial(IShader *shader, ITexture *texture)
{
    auto mat = std::make_unique<SWMaterial>(shader, texture);
    IMaterial *raw = mat.get();
    m_materials.push_back(std::move(mat));
    return raw;
}

void SoftwareRenderer::DestroyMaterial(IMaterial *material)
{
    if (const auto it = std::ranges::find_if(m_materials, [material](const auto &p) { return p.get() == material; });
        it != m_materials.end())
    {
        m_materials.erase(it);
    }
}

void SoftwareRenderer::SetViewProjection(const float *view, const float *proj)
{
    memcpy(m_view, view, 64);
    memcpy(m_proj, proj, 64);
}

void SoftwareRenderer::UpdateSceneLighting(const SceneLightingData &lighting, const N2Engine::Math::Vector3 &camPos)
{
    m_lighting = lighting;
    m_cameraPos = camPos;
}

void SoftwareRenderer::DrawMesh(IMesh* mesh, const float* modelMatrix, IMaterial* material)
{
    // Just record — don't rasterize yet
    auto* swMesh = dynamic_cast<SWMesh*>(mesh);
    auto* swMat  = dynamic_cast<SWMaterial*>(material);
    if (!swMesh || !swMat) return;

    DrawCommand cmd;
    cmd.mesh = swMesh;
    cmd.material = swMat;
    memcpy(cmd.modelMatrix, modelMatrix, 64);
    m_drawQueue.push_back(cmd);
}

void SoftwareRenderer::DrawObjects(const std::vector<RenderObject> &objects)
{
    for (const auto &obj : objects)
        DrawMesh(obj.mesh, obj.transform.model, obj.material);
}

void SoftwareRenderer::ReadFramebuffer(uint8_t *buffer, int width, int height) const
{
    // nearest-neighbour downsample/copy into the caller's buffer (RGBA8)
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            int sx = (int)((float)x / (float)width * (float)m_width);
            int sy = (int)((float)y / (float)height * (float)m_height);
            sx = std::clamp(sx, 0, (int)m_width - 1);
            sy = std::clamp(sy, 0, (int)m_height - 1);

            uint32_t packed = m_colorBuffer[(m_height - 1 - sy) * m_width + sx]; // flip Y
            uint8_t *dst = buffer + (y * width + x) * 4;
            dst[0] = (packed >> 0) & 0xFF; // R
            dst[1] = (packed >> 8) & 0xFF; // G
            dst[2] = (packed >> 16) & 0xFF; // B
            dst[3] = (packed >> 24) & 0xFF; // A
        }
}

void SoftwareRenderer::ClearBuffers()
{
    auto r = (uint8_t)(m_clearR * 255);
    auto g = (uint8_t)(m_clearG * 255);
    auto b = (uint8_t)(m_clearB * 255);
    auto a = (uint8_t)(m_clearA * 255);
    uint32_t bg = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    std::fill(m_colorBuffer.begin(), m_colorBuffer.end(), bg);
    std::fill(m_depthBuffer.begin(), m_depthBuffer.end(), 1.0f);
}

void SoftwareRenderer::SetPixel(int x, int y, float depth, uint32_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= m_width || (uint32_t)y >= m_height) return;
    int fy = (int)m_height - 1 - y;
    size_t idx = (size_t)fy * m_width + x;
    if (depth < m_depthBuffer[idx])
    {
        m_depthBuffer[idx] = depth;
        m_colorBuffer[idx] = color;
    }
}

void SoftwareRenderer::RasterizeTriangle(const SWFragment &f0, const SWFragment &f1,
                                         const SWFragment &f2, const SWMaterial *mat,
                                         const float *modelMatrix)
{
    auto ndcToScreen = [&](float nx, float ny, float &sx, float &sy)
    {
        sx = (nx + 1.f) * 0.5f * (float)m_width;
        sy = (ny + 1.f) * 0.5f * (float)m_height;
    };

    float x0, y0, x1, y1, x2, y2;
    ndcToScreen(f0.x, f0.y, x0, y0);
    ndcToScreen(f1.x, f1.y, x1, y1);
    ndcToScreen(f2.x, f2.y, x2, y2);

    int minX = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
    int maxX = std::min((int)m_width - 1, (int)std::ceil(std::max({x0, x1, x2})));
    int minY = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
    int maxY = std::min((int)m_height - 1, (int)std::ceil(std::max({y0, y1, y2})));

    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 1e-6f) return;

    // Determine shading mode from material's shader type
    bool useLit = false;
    if (mat)
    {
        auto *sw = dynamic_cast<const SWShader*>(mat->GetShader());
        useLit = sw && sw->GetType() == SWShaderType::Lit;
    }

    for (int py = minY; py <= maxY; ++py)
        for (int px = minX; px <= maxX; ++px)
        {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;

            float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / denom;
            float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / denom;
            float w2 = 1.f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;

            // Interpolate
            SWFragment frag;
            frag.x  = w0 * f0.x  + w1 * f1.x  + w2 * f2.x;
            frag.y  = w0 * f0.y  + w1 * f1.y  + w2 * f2.y;
            frag.z  = w0 * f0.z  + w1 * f1.z  + w2 * f2.z;
            frag.wx = w0 * f0.wx + w1 * f1.wx + w2 * f2.wx;
            frag.wy = w0 * f0.wy + w1 * f1.wy + w2 * f2.wy;
            frag.wz = w0 * f0.wz + w1 * f1.wz + w2 * f2.wz;
            frag.nx = w0 * f0.nx + w1 * f1.nx + w2 * f2.nx;
            frag.ny = w0 * f0.ny + w1 * f1.ny + w2 * f2.ny;
            frag.nz = w0 * f0.nz + w1 * f1.nz + w2 * f2.nz;
            frag.u  = w0 * f0.u  + w1 * f1.u  + w2 * f2.u;
            frag.v  = w0 * f0.v  + w1 * f1.v  + w2 * f2.v;
            frag.r  = w0 * f0.r  + w1 * f1.r  + w2 * f2.r;
            frag.g  = w0 * f0.g  + w1 * f1.g  + w2 * f2.g;
            frag.b  = w0 * f0.b  + w1 * f1.b  + w2 * f2.b;
            frag.a  = w0 * f0.a  + w1 * f1.a  + w2 * f2.a;

            uint32_t color = useLit
                                 ? ShadeLit(frag, mat, modelMatrix)
                                 : ShadeUnlit(frag, mat);
            SetPixel(px, py, frag.z, color);
        }
}

namespace
{
    // Per-vertex data carried through clipping. Lerping a ClipVertex linearly in
    // clip space is correct because clip-space lerp = perspective-correct interp.
    struct ClipVertex
    {
        float c[4];   // clip-space position (x, y, z, w)
        float wp[3];  // world-space position
        float wn[3];  // world-space normal
        float uv[2];  // texcoord
        float col[4]; // vertex color
    };

    ClipVertex LerpClipVertex(const ClipVertex& a, const ClipVertex& b, float t)
    {
        ClipVertex r;
        for (int i = 0; i < 4; ++i) r.c[i]   = a.c[i]   + t * (b.c[i]   - a.c[i]);
        for (int i = 0; i < 3; ++i) r.wp[i]  = a.wp[i]  + t * (b.wp[i]  - a.wp[i]);
        for (int i = 0; i < 3; ++i) r.wn[i]  = a.wn[i]  + t * (b.wn[i]  - a.wn[i]);
        for (int i = 0; i < 2; ++i) r.uv[i]  = a.uv[i]  + t * (b.uv[i]  - a.uv[i]);
        for (int i = 0; i < 4; ++i) r.col[i] = a.col[i] + t * (b.col[i] - a.col[i]);
        return r;
    }
}

void SoftwareRenderer::RasterizeMesh(SWMesh* mesh, const float* modelMatrix, SWMaterial* material)
{
    if (!mesh || !mesh->IsValid()) return;

    float mv[16], mvp[16];
    Mul4x4(m_view, modelMatrix, mv);
    Mul4x4(m_proj, mv, mvp);

    const auto& verts   = mesh->vertices;
    const auto& indices = mesh->indices;

    auto buildClipVertex = [&](const Vertex& v) -> ClipVertex
    {
        ClipVertex cv;
        TransformPoint(mvp, v.position, cv.c);

        float wp4[4];
        TransformPoint(modelMatrix, v.position, wp4);
        cv.wp[0] = wp4[0]; cv.wp[1] = wp4[1]; cv.wp[2] = wp4[2];

        TransformNormal(modelMatrix, v.normal, cv.wn);

        cv.uv[0] = v.texCoord[0]; cv.uv[1] = v.texCoord[1];
        cv.col[0] = v.color[0]; cv.col[1] = v.color[1];
        cv.col[2] = v.color[2]; cv.col[3] = v.color[3];
        return cv;
    };

    auto toFragment = [](const ClipVertex& cv) -> SWFragment
    {
        float invW = (std::abs(cv.c[3]) > 1e-6f) ? 1.f / cv.c[3] : 0.f;
        SWFragment f;
        f.x = cv.c[0] * invW;
        f.y = cv.c[1] * invW;
        f.z = cv.c[2] * invW;
        f.wx = cv.wp[0]; f.wy = cv.wp[1]; f.wz = cv.wp[2];
        f.nx = cv.wn[0]; f.ny = cv.wn[1]; f.nz = cv.wn[2];
        f.u  = cv.uv[0]; f.v  = cv.uv[1];
        f.r  = cv.col[0]; f.g = cv.col[1]; f.b = cv.col[2]; f.a = cv.col[3];
        return f;
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        ClipVertex in[3] = {
            buildClipVertex(verts[indices[i + 0]]),
            buildClipVertex(verts[indices[i + 1]]),
            buildClipVertex(verts[indices[i + 2]])
        };

        // Near-plane sign distance in clip space: inside iff z + w >= 0
        float d[3] = { in[0].c[2] + in[0].c[3],
                       in[1].c[2] + in[1].c[3],
                       in[2].c[2] + in[2].c[3] };

        int insideMask = (d[0] >= 0 ? 1 : 0)
                       | (d[1] >= 0 ? 2 : 0)
                       | (d[2] >= 0 ? 4 : 0);

        ClipVertex out[4];
        int outCount = 0;

        if (insideMask == 0b111)
        {
            out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
            outCount = 3;
        }
        else if (insideMask == 0)
        {
            continue; // entirely behind near plane
        }
        else
        {
            // Sutherland–Hodgman against the near plane
            for (int e = 0; e < 3; ++e)
            {
                int a = e;
                int b = (e + 1) % 3;
                bool aIn = d[a] >= 0;
                bool bIn = d[b] >= 0;

                if (aIn) out[outCount++] = in[a];
                if (aIn != bIn)
                {
                    float t = d[a] / (d[a] - d[b]);
                    out[outCount++] = LerpClipVertex(in[a], in[b], t);
                }
            }
        }

        // Triangulate the clipped polygon as a fan
        for (int t = 1; t + 1 < outCount; ++t)
        {
            SWFragment f0 = toFragment(out[0]);
            SWFragment f1 = toFragment(out[t]);
            SWFragment f2 = toFragment(out[t + 1]);
            RasterizeTriangle(f0, f1, f2, material, modelMatrix);
        }
    }
}

uint32_t SoftwareRenderer::ShadeUnlit(const SWFragment &frag, const SWMaterial *mat) const
{
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    if (mat)
    {
        auto tint = mat->GetVec4("uAlbedo", {1, 1, 1, 1});
        r = tint[0];
        g = tint[1];
        b = tint[2];
        a = tint[3];

        auto *tex = dynamic_cast<const SWTexture*>(mat->GetTexture());
        if (tex && tex->IsValid())
        {
            uint32_t s = tex->Sample(frag.u, frag.v);
            r *= ((s >> 0) & 0xFF) / 255.f;
            g *= ((s >> 8) & 0xFF) / 255.f;
            b *= ((s >> 16) & 0xFF) / 255.f;
            a *= ((s >> 24) & 0xFF) / 255.f;
        }
    }

    auto clamp01 = [](float x) { return std::clamp(x, 0.f, 1.f); };
    return ((uint32_t)(clamp01(a) * 255) << 24) | ((uint32_t)(clamp01(b) * 255) << 16)
        | ((uint32_t)(clamp01(g) * 255) << 8) | (uint32_t)(clamp01(r) * 255);
}

uint32_t SoftwareRenderer::ShadeLit(const SWFragment& frag, const SWMaterial* mat,
                                     const float* /*modelMatrix*/) const
{
    // Normalize interpolated normal
    float nx = frag.nx, ny = frag.ny, nz = frag.nz;
    float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (nlen > 1e-6f) { nx /= nlen; ny /= nlen; nz /= nlen; }

    // Base color
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    float smoothness = 0.5f;
    if (mat)
    {
        auto albedo = mat->GetVec4("uAlbedo", {1, 1, 1, 1});
        r = albedo[0]; g = albedo[1]; b = albedo[2]; a = albedo[3];
        smoothness = mat->GetFloat("uSmoothness", 0.5f);

        auto* tex = dynamic_cast<const SWTexture*>(mat->GetTexture());
        if (tex && tex->IsValid())
        {
            uint32_t s = tex->Sample(frag.u, frag.v);
            r *= ((s >>  0) & 0xFF) / 255.f;
            g *= ((s >>  8) & 0xFF) / 255.f;
            b *= ((s >> 16) & 0xFF) / 255.f;
            a *= ((s >> 24) & 0xFF) / 255.f;
        }
    }

    // View vector (world space)
    float vx = m_cameraPos.x - frag.wx;
    float vy = m_cameraPos.y - frag.wy;
    float vz = m_cameraPos.z - frag.wz;
    float vlen = std::sqrt(vx*vx + vy*vy + vz*vz);
    if (vlen > 1e-6f) { vx /= vlen; vy /= vlen; vz /= vlen; }

    // Shininess from smoothness (matches mix(4, 256, smoothness) in GL)
    float shininess = 4.f + (256.f - 4.f) * smoothness;

    auto blinnSpec = [&](float lx, float ly, float lz) {
        float hx = lx + vx, hy = ly + vy, hz = lz + vz;
        float hlen = std::sqrt(hx*hx + hy*hy + hz*hz);
        if (hlen < 1e-6f) return 0.f;
        hx /= hlen; hy /= hlen; hz /= hlen;
        float ndoth = std::max(0.f, nx*hx + ny*hy + nz*hz);
        return std::pow(ndoth, shininess);
    };

    // Ambient
    float lr = m_lighting.ambientColor.x;
    float lg = m_lighting.ambientColor.y;
    float lb = m_lighting.ambientColor.z;

    // Directional lights
    for (const auto& dl : m_lighting.directionalLights)
    {
        float lx = -dl.direction.x, ly = -dl.direction.y, lz = -dl.direction.z;
        float len = std::sqrt(lx*lx + ly*ly + lz*lz);
        if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; }

        float ndotl = std::max(0.f, nx*lx + ny*ly + nz*lz);
        float diff = ndotl * dl.intensity;
        float spec = blinnSpec(lx, ly, lz) * 0.3f;

        lr += dl.color.x * (diff + spec);
        lg += dl.color.y * (diff + spec);
        lb += dl.color.z * (diff + spec);
    }

    // Point lights
    for (const auto& pl : m_lighting.pointLights)
    {
        float lx = pl.position.x - frag.wx;
        float ly = pl.position.y - frag.wy;
        float lz = pl.position.z - frag.wz;
        float dist = std::sqrt(lx*lx + ly*ly + lz*lz);
        if (dist > pl.range) continue;
        if (dist > 1e-6f) { lx /= dist; ly /= dist; lz /= dist; }

        float ndotl = std::max(0.f, nx*lx + ny*ly + nz*lz);
        // Matches GL: 1 / (1 + attenuation * (d/range)^2)
        float dr = dist / pl.range;
        float atten = 1.f / (1.f + pl.attenuation * dr * dr);
        float diff = ndotl * pl.intensity * atten;
        float spec = blinnSpec(lx, ly, lz) * 0.3f * atten;

        lr += pl.color.x * (diff + spec);
        lg += pl.color.y * (diff + spec);
        lb += pl.color.z * (diff + spec);
    }

    // Final: lighting * albedo
    lr *= r; lg *= g; lb *= b;

    auto clamp01 = [](float x){ return std::clamp(x, 0.f, 1.f); };
    return ((uint32_t)(clamp01(a)  * 255) << 24)
         | ((uint32_t)(clamp01(lb) * 255) << 16)
         | ((uint32_t)(clamp01(lg) * 255) <<  8)
         |  (uint32_t)(clamp01(lr) * 255);
}

void SoftwareRenderer::Mul4x4(const float *a, const float *b, float *out) const
{
    // Row-major: out[r,c] = sum_k a[r,k] * b[k,c]
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

void SoftwareRenderer::TransformPoint(const float *m, const float *in3, float *out4) const
{
    // Row-major: first row is m[0..3]
    out4[0] = m[ 0] * in3[0] + m[ 1] * in3[1] + m[ 2] * in3[2] + m[ 3];
    out4[1] = m[ 4] * in3[0] + m[ 5] * in3[1] + m[ 6] * in3[2] + m[ 7];
    out4[2] = m[ 8] * in3[0] + m[ 9] * in3[1] + m[10] * in3[2] + m[11];
    out4[3] = m[12] * in3[0] + m[13] * in3[1] + m[14] * in3[2] + m[15];
}

void SoftwareRenderer::TransformNormal(const float *model, const float *n, float *out) const
{
    // Upper-left 3x3, row-major. (No inverse-transpose — only valid for uniform scale.)
    out[0] = model[ 0] * n[0] + model[ 1] * n[1] + model[ 2] * n[2];
    out[1] = model[ 4] * n[0] + model[ 5] * n[1] + model[ 6] * n[2];
    out[2] = model[ 8] * n[0] + model[ 9] * n[1] + model[10] * n[2];
    float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-6f)
    {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}

bool SoftwareRenderer::SetupBlitResources()
{
    // Fullscreen quad — pos (xy) + uv
    constexpr float q[] = {
        -1, -1, 0, 1, 1, -1, 1, 1, 1, 1, 1, 0,
        -1, -1, 0, 1, 1, 1, 1, 0, -1, 1, 0, 0
    };
    glGenVertexArrays(1, &m_blitVAO);
    glGenBuffers(1, &m_blitVBO);
    glBindVertexArray(m_blitVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_blitVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenTextures(1, &m_blitTex);
    glBindTexture(GL_TEXTURE_2D, m_blitTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const char *vs = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){ vUV=aUV; gl_Position=vec4(aPos,0,1); })";

    const char *fs = R"(#version 330 core
in vec2 vUV; out vec4 frag;
uniform sampler2D uTex;
void main(){ frag = texture(uTex, vUV); })";

    auto compile = [](GLenum t, const char *src)
    {
        GLuint s = glCreateShader(t);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    GLuint sv = compile(GL_VERTEX_SHADER, vs);
    GLuint sf = compile(GL_FRAGMENT_SHADER, fs);
    m_blitProg = glCreateProgram();
    glAttachShader(m_blitProg, sv);
    glAttachShader(m_blitProg, sf);
    glLinkProgram(m_blitProg);
    glDeleteShader(sv);
    glDeleteShader(sf);
    glUseProgram(m_blitProg);
    glUniform1i(glGetUniformLocation(m_blitProg, "uTex"), 0);

    return true;
}

void SoftwareRenderer::SetWireframe(bool e) { m_wireframe = e; }