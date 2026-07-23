#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

#include "renderer/software/SWMaterial.hpp"
#include "renderer/software/SWMesh.hpp"
#include "renderer/software/SWShader.hpp"
#include "renderer/software/SWTexture.hpp"
#include "renderer/software/SoftwareRenderer.hpp"

using namespace Renderer;
using namespace Renderer::Common;
using namespace Renderer::Software;

// ============================================================================
// NOTE ON THE HEADER
// SoftwareRenderer::RasterizeTriangle / ShadeUnlit / ShadeLit are no longer
// defined in this file (the raster pipeline below replaces them with faster
// file-local machinery). Unreferenced private declarations link fine, so no
// header change is strictly required — but you can delete those three
// declarations (and SWFragment, if nothing else uses it) when convenient.
// SetPixel is kept unchanged in case anything else calls it.
// ============================================================================

// ============================================================================
// Tunables
// ============================================================================
namespace
{
    // Backface culling. GL convention: front faces wind counter-clockwise.
    // If your meshes suddenly disappear, first try kFrontFaceCCW = false;
    // if your content has mixed winding, set kCullBackfaces = false.
    constexpr bool kCullBackfaces = true;
    constexpr bool kFrontFaceCCW  = true;

    // Geometry is clipped against |x| <= kGuardBand*w and |y| <= kGuardBand*w
    // in clip space so projected coordinates stay bounded. This keeps the
    // fixed-point rasterizer overflow-free without ever clipping anything
    // visible (on-screen geometry satisfies |x| <= w).
    constexpr float kGuardBand = 8.0f;
    constexpr float kMinW      = 1e-6f;

    // 28.4 subpixel fixed point.
    constexpr int     kSubBits = 4;
    constexpr int32_t kSubStep = 1 << kSubBits;  // 16
    constexpr int32_t kSubHalf = kSubStep >> 1;  // 8
}

// ============================================================================
// Per-draw shading state, resolved ONCE per draw call.
// The old path did string-keyed uniform lookups and dynamic_casts per PIXEL;
// that cost dominated the actual shading math.
// ============================================================================
namespace
{
    inline uint32_t PackRGBA(float r, float g, float b, float a)
    {
        auto to8 = [](float x) -> uint32_t
        {
            x = std::clamp(x, 0.f, 1.f);
            return (uint32_t)(x * 255.f + 0.5f);
        };
        return (to8(a) << 24) | (to8(b) << 16) | (to8(g) << 8) | to8(r);
    }

    struct ResolvedMat
    {
        float aR = 1.f, aG = 1.f, aB = 1.f, aA = 1.f;
        float shininess = 130.f;
        const SWTexture* tex = nullptr;   // null if absent or invalid
        bool lit = false;
        uint32_t flatColor = 0xFFFFFFFF;  // unlit + untextured: constant per draw
    };

    ResolvedMat ResolveMaterial(const SWMaterial* mat)
    {
        ResolvedMat r;
        float smooth = 0.5f;
        if (mat)
        {
            auto alb = mat->GetVec4("uAlbedo", {1, 1, 1, 1});
            r.aR = alb[0]; r.aG = alb[1]; r.aB = alb[2]; r.aA = alb[3];
            smooth = mat->GetFloat("uSmoothness", 0.5f);

            if (auto* t = dynamic_cast<const SWTexture*>(mat->GetTexture()); t && t->IsValid())
                r.tex = t;

            auto* sh = dynamic_cast<const SWShader*>(mat->GetShader());
            r.lit = sh && sh->GetType() == SWShaderType::Lit;
        }
        r.shininess = 4.f + (256.f - 4.f) * smooth;   // matches mix(4, 256, smoothness)
        r.flatColor = PackRGBA(r.aR, r.aG, r.aB, r.aA);
        return r;
    }

    // Lights pre-normalized / pre-squared ONCE per draw instead of per pixel.
    struct PrepDirLight   { float x, y, z, r, g, b, intensity; };
    struct PrepPointLight { float x, y, z, r, g, b, intensity, range2, invRange, atten; };

    struct LitState
    {
        float ambR = 0.f, ambG = 0.f, ambB = 0.f;
        float camX = 0.f, camY = 0.f, camZ = 0.f;
        std::vector<PrepDirLight>   dirs;
        std::vector<PrepPointLight> points;
    };

    void PrepareLighting(const SceneLightingData& L, const N2Engine::Math::Vector3& cam, LitState& out)
    {
        out.ambR = L.ambientColor.x;
        out.ambG = L.ambientColor.y;
        out.ambB = L.ambientColor.z;
        out.camX = cam.x; out.camY = cam.y; out.camZ = cam.z;

        out.dirs.clear();
        out.dirs.reserve(L.directionalLights.size());
        for (const auto& dl : L.directionalLights)
        {
            float lx = -dl.direction.x, ly = -dl.direction.y, lz = -dl.direction.z;
            float len = std::sqrt(lx*lx + ly*ly + lz*lz);
            if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; }
            out.dirs.push_back({lx, ly, lz, dl.color.x, dl.color.y, dl.color.z, dl.intensity});
        }

        out.points.clear();
        out.points.reserve(L.pointLights.size());
        for (const auto& pl : L.pointLights)
        {
            if (pl.range <= 0.f) continue;   // old code rejected every pixel anyway
            out.points.push_back({pl.position.x, pl.position.y, pl.position.z,
                                  pl.color.x, pl.color.y, pl.color.z,
                                  pl.intensity, pl.range * pl.range, 1.f / pl.range,
                                  pl.attenuation});
        }
    }

    // ------------------------------------------------------------------
    // Per-pixel shading. Same math as before, but everything variable was
    // hoisted into ResolvedMat / LitState, and pow() only runs when N·H > 0.
    // ------------------------------------------------------------------
    inline uint32_t ShadeUnlitPx(float u, float v, const ResolvedMat& m)
    {
        // Caller guarantees m.tex != nullptr (the flat case never reaches here).
        const uint32_t s = m.tex->Sample(u, v);
        constexpr float k = 1.f / 255.f;
        const float r = m.aR * (float)((s >>  0) & 0xFF) * k;
        const float g = m.aG * (float)((s >>  8) & 0xFF) * k;
        const float b = m.aB * (float)((s >> 16) & 0xFF) * k;
        const float a = m.aA * (float)((s >> 24) & 0xFF) * k;
        return PackRGBA(r, g, b, a);
    }

    inline uint32_t ShadeLitPx(float wx, float wy, float wz,
                               float nx, float ny, float nz,
                               float u, float v,
                               const ResolvedMat& m, const LitState& L)
    {
        const float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nlen > 1e-6f) { const float inv = 1.f / nlen; nx *= inv; ny *= inv; nz *= inv; }

        float r = m.aR, g = m.aG, b = m.aB, a = m.aA;
        if (m.tex)
        {
            const uint32_t s = m.tex->Sample(u, v);
            constexpr float k = 1.f / 255.f;
            r *= (float)((s >>  0) & 0xFF) * k;
            g *= (float)((s >>  8) & 0xFF) * k;
            b *= (float)((s >> 16) & 0xFF) * k;
            a *= (float)((s >> 24) & 0xFF) * k;
        }

        float vx = L.camX - wx, vy = L.camY - wy, vz = L.camZ - wz;
        const float vlen = std::sqrt(vx*vx + vy*vy + vz*vz);
        if (vlen > 1e-6f) { const float inv = 1.f / vlen; vx *= inv; vy *= inv; vz *= inv; }

        auto blinn = [&](float lx, float ly, float lz) -> float
        {
            const float hx = lx + vx, hy = ly + vy, hz = lz + vz;
            const float h2 = hx*hx + hy*hy + hz*hz;
            if (h2 < 1e-12f) return 0.f;
            float ndoth = nx*hx + ny*hy + nz*hz;
            if (ndoth <= 0.f) return 0.f;               // skip pow() when it can't contribute
            ndoth /= std::sqrt(h2);
            return std::pow(ndoth, m.shininess);
        };

        float lr = L.ambR, lg = L.ambG, lb = L.ambB;

        for (const auto& d : L.dirs)
        {
            const float ndotl = std::max(0.f, nx*d.x + ny*d.y + nz*d.z);
            // Kept from the original / GL shader: spec is NOT gated on N·L.
            const float c = ndotl * d.intensity + blinn(d.x, d.y, d.z) * 0.3f;
            lr += d.r * c; lg += d.g * c; lb += d.b * c;
        }

        for (const auto& p : L.points)
        {
            float lx = p.x - wx, ly = p.y - wy, lz = p.z - wz;
            const float d2 = lx*lx + ly*ly + lz*lz;
            if (d2 > p.range2) continue;                 // reject before the sqrt
            const float dist = std::sqrt(d2);
            if (dist > 1e-6f) { const float inv = 1.f / dist; lx *= inv; ly *= inv; lz *= inv; }

            const float ndotl = std::max(0.f, nx*lx + ny*ly + nz*lz);
            const float dr    = dist * p.invRange;
            const float atten = 1.f / (1.f + p.atten * dr * dr);
            const float c = (ndotl * p.intensity + blinn(lx, ly, lz) * 0.3f) * atten;
            lr += p.r * c; lg += p.g * c; lb += p.b * c;
        }

        return PackRGBA(lr * r, lg * g, lb * b, a);
    }
}

// ============================================================================
// Clipping. Lerping in clip space (before the perspective divide) is exact,
// so clip-produced vertices need no special treatment. In addition to the
// near plane we clip against w >= kMinW and a screen-aligned guard band —
// this bounds projected coordinates (fixed-point safety) and removes the
// old invW = 0 degenerate fallback.
// ============================================================================
namespace
{
    struct ClipVertex
    {
        float c[4];   // clip-space position
        float wp[3];  // world-space position
        float wn[3];  // world-space normal
        float uv[2];  // texcoord
        // NOTE: vertex colors are no longer carried through the pipeline —
        // neither shader ever read them, so interpolating them was pure waste.
    };

    inline ClipVertex LerpCV(const ClipVertex& a, const ClipVertex& b, float t)
    {
        ClipVertex r;
        for (int i = 0; i < 4; ++i) r.c[i]  = a.c[i]  + t * (b.c[i]  - a.c[i]);
        for (int i = 0; i < 3; ++i) r.wp[i] = a.wp[i] + t * (b.wp[i] - a.wp[i]);
        for (int i = 0; i < 3; ++i) r.wn[i] = a.wn[i] + t * (b.wn[i] - a.wn[i]);
        for (int i = 0; i < 2; ++i) r.uv[i] = a.uv[i] + t * (b.uv[i] - a.uv[i]);
        return r;
    }

    enum : uint32_t
    {
        OC_W = 1, OC_NEAR = 2, OC_LEFT = 4, OC_RIGHT = 8, OC_BOTTOM = 16, OC_TOP = 32
    };

    inline uint32_t Outcode(const float c[4])
    {
        uint32_t oc = 0;
        const float w  = c[3];
        const float gw = kGuardBand * w;
        if (w < kMinW)      oc |= OC_W;
        if (c[2] + w < 0.f) oc |= OC_NEAR;
        if (c[0] < -gw)     oc |= OC_LEFT;
        if (c[0] >  gw)     oc |= OC_RIGHT;
        if (c[1] < -gw)     oc |= OC_BOTTOM;
        if (c[1] >  gw)     oc |= OC_TOP;
        return oc;
    }

    template <typename DistFn>
    int ClipEdge(const ClipVertex* in, int n, ClipVertex* out, DistFn&& dist)
    {
        int m = 0;
        for (int i = 0; i < n; ++i)
        {
            const ClipVertex& A = in[i];
            const ClipVertex& B = in[(i + 1) % n];
            const float da = dist(A), db = dist(B);
            const bool aIn = da >= 0.f, bIn = db >= 0.f;
            if (aIn) out[m++] = A;
            if (aIn != bIn) out[m++] = LerpCV(A, B, da / (da - db));
        }
        return m;
    }

    // Sutherland–Hodgman, but only against the planes some vertex actually
    // violates (ocUnion). Returns the clipped vertex count (0 if culled).
    int ClipTriangle(const ClipVertex tri[3], uint32_t ocUnion, ClipVertex* out)
    {
        ClipVertex buf[2][12];   // 3 verts + up to 1 per plane * 6 planes = 9 max
        buf[0][0] = tri[0]; buf[0][1] = tri[1]; buf[0][2] = tri[2];
        int n = 3, src = 0;

        auto pass = [&](uint32_t bit, auto&& dist)
        {
            if ((ocUnion & bit) && n >= 3)
            {
                n = ClipEdge(buf[src], n, buf[src ^ 1], dist);
                src ^= 1;
            }
        };
        pass(OC_W,      [](const ClipVertex& v) { return v.c[3] - kMinW; });
        pass(OC_NEAR,   [](const ClipVertex& v) { return v.c[2] + v.c[3]; });
        pass(OC_LEFT,   [](const ClipVertex& v) { return kGuardBand * v.c[3] + v.c[0]; });
        pass(OC_RIGHT,  [](const ClipVertex& v) { return kGuardBand * v.c[3] - v.c[0]; });
        pass(OC_BOTTOM, [](const ClipVertex& v) { return kGuardBand * v.c[3] + v.c[1]; });
        pass(OC_TOP,    [](const ClipVertex& v) { return kGuardBand * v.c[3] - v.c[1]; });

        if (n < 3) return 0;
        for (int i = 0; i < n; ++i) out[i] = buf[src][i];
        return n;
    }
}

// ============================================================================
// Rasterization.
//   * 28.4 fixed-point edge functions, evaluated incrementally (adds per
//     pixel instead of full barycentric recomputation), with a watertight
//     fill rule — shared edges own their boundary pixels exactly once.
//   * Early-Z: only depth is interpolated before the depth test; attributes
//     and shading run only for surviving pixels.
//   * Perspective-correct interpolation: A/w and 1/w are interpolated with
//     screen-space barycentrics and A recovered per pixel. The old affine
//     path made textures swim on perspective-heavy triangles. (NDC depth is
//     affine in screen space, so the depth path is unchanged and exact.)
// ============================================================================
namespace
{
    struct ScreenVert
    {
        int32_t fx, fy;            // 28.4 fixed-point window coordinates
        float z;                   // NDC z, used directly for depth
        float invW;
        float uw, vw;              // u/w, v/w
        float nxw, nyw, nzw;       // world normal / w
        float wxw, wyw, wzw;       // world position / w
    };

    inline ScreenVert Project(const ClipVertex& v, float halfW, float halfH)
    {
        const float invW = 1.f / v.c[3];   // w >= kMinW is guaranteed after clipping
        ScreenVert s;
        s.fx   = (int32_t)std::lround((v.c[0] * invW + 1.f) * halfW * (float)kSubStep);
        s.fy   = (int32_t)std::lround((v.c[1] * invW + 1.f) * halfH * (float)kSubStep);
        s.z    = v.c[2] * invW;
        s.invW = invW;
        s.uw  = v.uv[0] * invW;  s.vw  = v.uv[1] * invW;
        s.nxw = v.wn[0] * invW;  s.nyw = v.wn[1] * invW;  s.nzw = v.wn[2] * invW;
        s.wxw = v.wp[0] * invW;  s.wyw = v.wp[1] * invW;  s.wzw = v.wp[2] * invW;
        return s;
    }

    struct RasterTarget
    {
        uint32_t* color;
        float*    depth;
        int       width, height;
    };

    inline int64_t Orient(const ScreenVert& a, const ScreenVert& b, const ScreenVert& c)
    {
        return (int64_t)(b.fx - a.fx) * (c.fy - a.fy)
             - (int64_t)(b.fy - a.fy) * (c.fx - a.fx);
    }

    template <bool LIT>
    void RasterTri(const ScreenVert& sv0, const ScreenVert& sv1, const ScreenVert& sv2,
                   const RasterTarget& t, const ResolvedMat& mat,
                   [[maybe_unused]] const LitState& lit)
    {
        const ScreenVert* A = &sv0;
        const ScreenVert* B = &sv1;
        const ScreenVert* C = &sv2;

        int64_t area2 = Orient(*A, *B, *C);   // 2x signed area; sign = winding
        if (area2 == 0) return;

        if constexpr (kCullBackfaces)
        {
            const bool front = kFrontFaceCCW ? (area2 > 0) : (area2 < 0);
            if (!front) return;
        }
        if (area2 < 0) { std::swap(B, C); area2 = -area2; }   // canonicalize to CCW

        // Bounding box of covered pixel CENTERS (centers sit at px*16 + 8).
        const int32_t minFx = std::min({A->fx, B->fx, C->fx});
        const int32_t maxFx = std::max({A->fx, B->fx, C->fx});
        const int32_t minFy = std::min({A->fy, B->fy, C->fy});
        const int32_t maxFy = std::max({A->fy, B->fy, C->fy});

        int px0 = (minFx - kSubHalf + kSubStep - 1) >> kSubBits;   // ceil
        int px1 = (maxFx - kSubHalf) >> kSubBits;                  // floor
        int py0 = (minFy - kSubHalf + kSubStep - 1) >> kSubBits;
        int py1 = (maxFy - kSubHalf) >> kSubBits;

        px0 = std::max(px0, 0);
        py0 = std::max(py0, 0);
        px1 = std::min(px1, t.width  - 1);
        py1 = std::min(py1, t.height - 1);
        if (px0 > px1 || py0 > py1) return;

        // Edge setup. The fill-rule bias makes a shared edge belong to exactly
        // one of its two triangles, so adjacent triangles neither double-shade
        // nor leave cracks along shared edges.
        auto edgeSetup = [](const ScreenVert& a, const ScreenVert& b,
                            int64_t& stepX, int64_t& stepY, int64_t& bias)
        {
            const int32_t dx = b.fx - a.fx;
            const int32_t dy = b.fy - a.fy;
            stepX = -(int64_t)dy * kSubStep;   // per +1 pixel in x
            stepY =  (int64_t)dx * kSubStep;   // per +1 pixel in y
            const bool acceptsEq = (dy < 0) || (dy == 0 && dx > 0);
            bias = acceptsEq ? 0 : 1;
        };
        auto edgeAt = [](const ScreenVert& a, const ScreenVert& b, int64_t cx, int64_t cy)
        {
            return (int64_t)(b.fx - a.fx) * (cy - a.fy)
                 - (int64_t)(b.fy - a.fy) * (cx - a.fx);
        };

        int64_t s0x, s0y, b0, s1x, s1y, b1, s2x, s2y, b2;
        edgeSetup(*B, *C, s0x, s0y, b0);   // weight of A
        edgeSetup(*C, *A, s1x, s1y, b1);   // weight of B
        edgeSetup(*A, *B, s2x, s2y, b2);   // weight of C

        const int64_t cx0 = (int64_t)px0 * kSubStep + kSubHalf;
        const int64_t cy0 = (int64_t)py0 * kSubStep + kSubHalf;

        // Bias folded in: the inside test collapses to (e0|e1|e2) >= 0.
        int64_t r0 = edgeAt(*B, *C, cx0, cy0) - b0;
        int64_t r1 = edgeAt(*C, *A, cx0, cy0) - b1;
        int64_t r2 = edgeAt(*A, *B, cx0, cy0) - b2;

        const float invArea = 1.f / (float)area2;
        const float zA = A->z, zB = B->z, zC = C->z;

        for (int py = py0; py <= py1; ++py)
        {
            // Y-flip folded into the row base (row 0 = bottom of the screen).
            const size_t row = (size_t)(t.height - 1 - py) * (size_t)t.width;
            uint32_t* crow = t.color + row;
            float*    drow = t.depth + row;

            int64_t e0 = r0, e1 = r1, e2 = r2;

            for (int px = px0; px <= px1; ++px)
            {
                if ((e0 | e1 | e2) >= 0)   // sign-bit trick: inside iff none negative
                {
                    const float l0 = (float)(e0 + b0) * invArea;
                    const float l1 = (float)(e1 + b1) * invArea;
                    const float l2 = (float)(e2 + b2) * invArea;

                    // Early-Z: interpolate depth only; shade only survivors.
                    const float z = l0 * zA + l1 * zB + l2 * zC;
                    if (z < drow[px])
                    {
                        uint32_t colOut;
                        if constexpr (LIT)
                        {
                            const float iw = l0*A->invW + l1*B->invW + l2*C->invW;
                            const float rw = 1.f / iw;
                            const float u  = (l0*A->uw  + l1*B->uw  + l2*C->uw ) * rw;
                            const float v  = (l0*A->vw  + l1*B->vw  + l2*C->vw ) * rw;
                            const float nx = (l0*A->nxw + l1*B->nxw + l2*C->nxw) * rw;
                            const float ny = (l0*A->nyw + l1*B->nyw + l2*C->nyw) * rw;
                            const float nz = (l0*A->nzw + l1*B->nzw + l2*C->nzw) * rw;
                            const float wx = (l0*A->wxw + l1*B->wxw + l2*C->wxw) * rw;
                            const float wy = (l0*A->wyw + l1*B->wyw + l2*C->wyw) * rw;
                            const float wz = (l0*A->wzw + l1*B->wzw + l2*C->wzw) * rw;
                            colOut = ShadeLitPx(wx, wy, wz, nx, ny, nz, u, v, mat, lit);
                        }
                        else if (mat.tex)
                        {
                            const float iw = l0*A->invW + l1*B->invW + l2*C->invW;
                            const float rw = 1.f / iw;
                            const float u  = (l0*A->uw + l1*B->uw + l2*C->uw) * rw;
                            const float v  = (l0*A->vw + l1*B->vw + l2*C->vw) * rw;
                            colOut = ShadeUnlitPx(u, v, mat);
                        }
                        else
                        {
                            colOut = mat.flatColor;   // constant per draw — no interpolation at all
                        }
                        drow[px] = z;
                        crow[px] = colOut;
                    }
                }
                e0 += s0x; e1 += s1x; e2 += s2x;
            }
            r0 += s0y; r1 += s1y; r2 += s2y;
        }
    }
}

// ============================================================================
// Renderer
// ============================================================================

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
    // CAUTION (pre-existing): if a frame is in flight on the render thread,
    // reallocating these buffers races with it. Safest call sites are before
    // EndFrame or after Present. A per-frame buffer mutex or a WaitForFrame
    // here would make this airtight.
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
    // Snapshot everything the render thread needs. The queue is MOVED, not
    // copied (the old code deep-copied it every frame). Camera position and
    // clear color are now part of the snapshot too — previously specular
    // highlights could read a different frame's camera.
    auto queue = std::move(m_drawQueue);
    m_drawQueue.clear();   // moved-from -> guaranteed empty and reusable

    std::array<float, 16> view{}, proj{};
    std::memcpy(view.data(), m_view, 64);
    std::memcpy(proj.data(), m_proj, 64);
    auto lighting = m_lighting;
    auto camPos   = m_cameraPos;
    const float cr = m_clearR, cg = m_clearG, cb = m_clearB, ca = m_clearA;

    m_renderThread.SubmitFrame({
        [this, queue = std::move(queue), view, proj,
         lighting = std::move(lighting), camPos, cr, cg, cb, ca]() mutable
        {
            m_clearR = cr; m_clearG = cg; m_clearB = cb; m_clearA = ca;
            ClearBuffers();

            std::memcpy(m_view, view.data(), 64);
            std::memcpy(m_proj, proj.data(), 64);
            m_lighting  = std::move(lighting);
            m_cameraPos = camPos;

            // Sort opaque draws front-to-back so early-Z rejects occluded
            // pixels before they're shaded — overdraw becomes nearly free.
            // (No alpha blending exists, so draw order can't change the image.)
            std::sort(queue.begin(), queue.end(),
                      [&](const DrawCommand& a, const DrawCommand& b)
                      {
                          auto dist2 = [&](const DrawCommand& c)
                          {
                              // Row-major model matrix: translation at [3], [7], [11].
                              const float dx = c.modelMatrix[3]  - camPos.x;
                              const float dy = c.modelMatrix[7]  - camPos.y;
                              const float dz = c.modelMatrix[11] - camPos.z;
                              return dx*dx + dy*dy + dz*dz;
                          };
                          return dist2(a) < dist2(b);
                      });

            for (auto& cmd : queue)
                RasterizeMesh(cmd.mesh, cmd.modelMatrix, cmd.material);

            // NOTE: GL upload stays in Present() — GL context lives on the main thread.
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
    {
        int sy = (int)((float)y / (float)height * (float)m_height);
        sy = std::clamp(sy, 0, (int)m_height - 1);
        const uint32_t* srcRow = m_colorBuffer.data() + (size_t)(m_height - 1 - sy) * m_width; // flip Y
        uint8_t* dstRow = buffer + (size_t)y * width * 4;

        for (int x = 0; x < width; ++x)
        {
            int sx = (int)((float)x / (float)width * (float)m_width);
            sx = std::clamp(sx, 0, (int)m_width - 1);

            const uint32_t packed = srcRow[sx];
            uint8_t *dst = dstRow + (size_t)x * 4;
            dst[0] = (packed >> 0) & 0xFF; // R
            dst[1] = (packed >> 8) & 0xFF; // G
            dst[2] = (packed >> 16) & 0xFF; // B
            dst[3] = (packed >> 24) & 0xFF; // A
        }
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

// Kept for external callers / debug draws. The raster path no longer uses it —
// the inner loop writes rows directly with the Y flip folded into the row base.
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

void SoftwareRenderer::RasterizeMesh(SWMesh* mesh, const float* modelMatrix, SWMaterial* material)
{
    if (!mesh || !mesh->IsValid()) return;
    if (m_width == 0 || m_height == 0) return;

    float mv[16], mvp[16];
    Mul4x4(m_view, modelMatrix, mv);
    Mul4x4(m_proj, mv, mvp);

    // Everything the pixel loop needs, resolved ONCE per draw. The old path
    // paid string-keyed uniform lookups and dynamic_casts per pixel.
    const ResolvedMat rm = ResolveMaterial(material);

    LitState lit;
    if (rm.lit)
        PrepareLighting(m_lighting, m_cameraPos, lit);   // normalize lights once, not per pixel

    const RasterTarget target{ m_colorBuffer.data(), m_depthBuffer.data(),
                               (int)m_width, (int)m_height };
    const float halfW = 0.5f * (float)m_width;
    const float halfH = 0.5f * (float)m_height;

    const auto& verts   = mesh->vertices;
    const auto& indices = mesh->indices;

    // Transform each unique vertex ONCE. The old path re-transformed per
    // index — up to ~6x per vertex on typical meshes. Scratch is reused
    // across draw calls (single render thread; thread_local for safety).
    struct XfVert { ClipVertex cv; ScreenVert sv; uint32_t oc; };
    static thread_local std::vector<XfVert> s_xf;
    s_xf.resize(verts.size());

    for (size_t i = 0; i < verts.size(); ++i)
    {
        const auto& v = verts[i];
        XfVert& x = s_xf[i];

        TransformPoint(mvp, v.position, x.cv.c);

        float wp4[4];
        TransformPoint(modelMatrix, v.position, wp4);
        x.cv.wp[0] = wp4[0]; x.cv.wp[1] = wp4[1]; x.cv.wp[2] = wp4[2];

        TransformNormal(modelMatrix, v.normal, x.cv.wn);

        x.cv.uv[0] = v.texCoord[0];
        x.cv.uv[1] = v.texCoord[1];

        x.oc = Outcode(x.cv.c);
        if (x.oc == 0)
            x.sv = Project(x.cv, halfW, halfH);   // project once; reused by every triangle sharing it
    }

    auto raster = [&](const ScreenVert& a, const ScreenVert& b, const ScreenVert& c)
    {
        if (rm.lit) RasterTri<true >(a, b, c, target, rm, lit);
        else        RasterTri<false>(a, b, c, target, rm, lit);
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const XfVert& v0 = s_xf[indices[i + 0]];
        const XfVert& v1 = s_xf[indices[i + 1]];
        const XfVert& v2 = s_xf[indices[i + 2]];

        if (v0.oc & v1.oc & v2.oc) continue;            // all outside one plane — trivial reject

        const uint32_t ocUnion = v0.oc | v1.oc | v2.oc;
        if (ocUnion == 0)                               // fully inside — no clipping needed
        {
            raster(v0.sv, v1.sv, v2.sv);
            continue;
        }

        const ClipVertex tri[3] = { v0.cv, v1.cv, v2.cv };
        ClipVertex poly[12];
        const int n = ClipTriangle(tri, ocUnion, poly);

        ScreenVert sv[12];
        for (int k = 0; k < n; ++k) sv[k] = Project(poly[k], halfW, halfH);
        for (int k = 1; k + 1 < n; ++k)                 // fan-triangulate
            raster(sv[0], sv[k], sv[k + 1]);
    }
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

    const char *vs = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){ vUV=aUV; gl_Position=vec4(aPos,0,1); })";

    const char *fs = R"(
#version 330 core
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