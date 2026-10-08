#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Texture.hpp"

namespace Renderer::Common
{
    class IMaterial;
}

namespace N2Engine::Rendering
{
    /// How a material is lit: which of the renderer's standard shaders draws it
    enum class ShadingModel : std::uint8_t
    {
        Unlit, ///< the base colour as is (GetStandardUnlitShader)
        Lit,   ///< Blinn-Phong with the scene's lights (GetStandardLitShader)
        /// Metallic-roughness PBR (Cook-Torrance GGX) with the scene's lights, on the same standard lit shader:
        /// metallic, smoothness (1 - roughness) and the metallic-roughness texture shape the surface. Optional: no
        /// material has it unless it is asked for ("shading": "pbr", or a model imported with pbrMaterials).
        Pbr
    };

    /// How a material's alpha is used (glTF's alphaMode)
    enum class AlphaMode : std::uint8_t
    {
        Opaque, ///< alpha ignored for coverage: drawn in the Opaque queue, not blended
        Mask,   ///< cut out: pixels with alpha below alphaCutoff are discarded; the Opaque queue, not blended
        Blend   ///< blended over what is behind (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) in the Transparent queue
    };

    /**
     * A surface description (`"Material"`): what a MeshRenderer draws each submesh with. CPU data only; each
     * renderer gets one GPU material per Material and version through the engine's GPU cache
     * (GpuCache::AcquireMaterial), shared by every user.
     *
     * The colour drawn is baseColor x baseColorTexture (when set) x the vertex colour, on the standard unlit or
     * lit shader. The alpha mode picks the render queue and blending: Opaque and Mask draw in the Opaque queue
     * unblended (Mask also alpha-tests against alphaCutoff), Blend draws in the Transparent queue, blended. On the
     * software renderer, which can't blend, Blend draws opaque without writing depth.
     *
     * Lit materials also draw emissive (a colour, times the emissive texture when set) added after the lighting,
     * unaffected by it, and an occlusion texture that darkens the ambient light only (see GetOcclusionStrength).
     * Unlit materials ignore both. In linear lighting (RenderSettings::GetColorSpace) a lit material's base
     * colour and emissive textures are made as sRGB textures when their Texture settings say `srgb` (GpuCache),
     * so they decode when sampled.
     *
     * A lit or PBR material may have a normal map (tangent space, green up, scaled by normalScale): the surface
     * normal the lights see is perturbed by it, from each vertex's tangent (a mesh without tangents is drawn
     * without normal mapping). With the Pbr shading, metallic and smoothness (roughness = 1 - smoothness) are the
     * factors that the metallic-roughness texture (glTF's packing: green roughness, blue metallic) multiplies; with
     * Lit, metallic and that texture are stored but unused (Blinn-Phong has neither). The ambient light is a
     * uniform environment in the PBR model: there is no image based lighting.
     *
     * Materials come from `.mat` files (JSON, see Load) or Create at runtime. Every change bumps GetVersion. Only a
     * change to what the GPU material is made of (the shading, and the base colour, emissive, occlusion, normal and
     * metallic-roughness textures) bumps GetGpuVersion, and a renderer then makes a new GPU material for it on its next draw; every other field
     * reaches the GPU as a uniform (ApplyUniforms) or a render state, set per draw, so changing it every frame
     * costs nothing extra.
     */
    class Material final : public Base::Asset
    {
    public:
        Material() = default;

        /// A new material at runtime: white, opaque, single-sided, with the given shading
        [[nodiscard]] static std::shared_ptr<Material> Create(ShadingModel shading = ShadingModel::Lit);

        /// The material MeshRenderer draws a submesh with when its slot is empty: lit white. Shared and fixed.
        [[nodiscard]] static std::shared_ptr<const Material> GetDefault();
        /// Unlit white: what the built-in shapes (CubeRenderer, SphereRenderer, QuadRenderer) draw with when they
        /// have no material, tinted with their colour. Shared and fixed.
        [[nodiscard]] static std::shared_ptr<const Material> GetDefaultUnlit();

        /**
         * A material from `.mat` JSON (the object Load reads). Missing keys keep the defaults; a key with the
         * wrong type, an unknown value or an unknown name is ignored with a warning naming `debugName`. Texture
         * paths that aren't res:// or user:// paths (or asset UUIDs) are relative to `baseDirectory`. nullptr,
         * with an error, when `json` isn't an object.
         */
        [[nodiscard]] static std::shared_ptr<Material> FromJson(const nlohmann::json &json,
                                                                const std::filesystem::path &baseDirectory = {},
                                                                std::string_view debugName = "material");

        /// This material as `.mat` JSON: every field, textures as their res:// path (or source file, or UUID for
        /// a texture made at runtime), null when unset
        [[nodiscard]] nlohmann::json ToJson() const;

        /// Registers the .mat loader with Resources (and so ResourceLoader). Material.cpp already does this at
        /// static initialisation, and ResourceLoader::Initialize calls it too; calling it again is harmless.
        static void RegisterLoader();

        /**
         * Loads a `.mat` file: a JSON object such as
         *   {"shading": "lit", "baseColor": {"r": 1, "g": 1, "b": 1, "a": 1},
         *    "baseColorTexture": "res://textures/crate.png", "alphaMode": "mask", "alphaCutoff": 0.5,
         *    "doubleSided": false, "smoothness": 0.5, "metallic": 0, "emissive": {"r": 0, "g": 0, "b": 0, "a": 1},
         *    "normalTexture": null, "normalScale": 1, "occlusionTexture": null, "occlusionStrength": 1,
         *    "metallicRoughnessTexture": null, "emissiveTexture": null}
         * See FromJson for how keys are read. False, with an error, if the file can't be read or isn't a JSON
         * object.
         */
        bool Load(const std::filesystem::path &path) override;
        /// What GetResourceType() returns, for code that needs it without an instance (asset metadata, a field's asset type)
        static constexpr std::string_view ResourceTypeName = "Material";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] ShadingModel GetShading() const { return _shading; }
        void SetShading(ShadingModel shading);
        [[nodiscard]] const Common::Color &GetBaseColor() const { return _baseColor; }
        void SetBaseColor(const Common::Color &color);
        [[nodiscard]] const std::shared_ptr<Texture> &GetBaseColorTexture() const { return _baseColorTexture; }
        void SetBaseColorTexture(std::shared_ptr<Texture> texture);
        [[nodiscard]] AlphaMode GetAlphaMode() const { return _alphaMode; }
        void SetAlphaMode(AlphaMode mode);
        /// Mask only: pixels with alpha below it are discarded. 0.5 by default; clamped to 0..1.
        [[nodiscard]] float GetAlphaCutoff() const { return _alphaCutoff; }
        void SetAlphaCutoff(float cutoff);
        /// Draws both faces (no culling)
        [[nodiscard]] bool IsDoubleSided() const { return _doubleSided; }
        void SetDoubleSided(bool doubleSided);
        /// Lit and Pbr: 0 (dull, wide highlights) to 1 (glossy, tight highlights); 0.5 by default. In PBR the
        /// roughness is 1 - smoothness.
        [[nodiscard]] float GetSmoothness() const { return _smoothness; }
        void SetSmoothness(float smoothness);

        /// Lit only: the colour the surface gives off, added after the lighting (so lights don't change it); rgb
        /// only, alpha is ignored. Black (the default) adds nothing. Values above 1 are allowed (brighter than
        /// the screen shows, but they still add to the lighting). The number is used as it is in both colour
        /// spaces (see RenderSettings).
        [[nodiscard]] const Common::Color &GetEmissive() const { return _emissive; }
        void SetEmissive(const Common::Color &emissive);
        /// Lit only: an image the emissive colour is multiplied by (glTF's emissiveTexture). Set the colour to
        /// white to show it as is: the default colour is black, which shows nothing.
        [[nodiscard]] const std::shared_ptr<Texture> &GetEmissiveTexture() const { return _emissiveTexture; }
        void SetEmissiveTexture(std::shared_ptr<Texture> texture);
        /// Lit only: an image whose red channel (1 = open, 0 = fully occluded) scales the ambient light; direct
        /// lights are not occluded (glTF's occlusionTexture). It is data, never decoded as sRGB.
        [[nodiscard]] const std::shared_ptr<Texture> &GetOcclusionTexture() const { return _occlusionTexture; }
        void SetOcclusionTexture(std::shared_ptr<Texture> texture);
        /// How much of the occlusion texture applies, from 0 (none) to 1 (all, the default): the ambient light is
        /// scaled by 1 + strength * (occlusion - 1)
        [[nodiscard]] float GetOcclusionStrength() const { return _occlusionStrength; }
        void SetOcclusionStrength(float strength);

        /// Pbr only: 0 (a dielectric) to 1 (a metal), times the metallic-roughness texture's blue channel; 0 by
        /// default. Stored (glTF's metallicFactor) but not used by Lit.
        [[nodiscard]] float GetMetallic() const { return _metallic; }
        void SetMetallic(float metallic);
        /// Lit and Pbr: a tangent-space normal map (glTF's normalTexture; red right, green up, blue out; data,
        /// never decoded as sRGB). It needs vertex tangents (see Renderer::Common::Vertex).
        [[nodiscard]] const std::shared_ptr<Texture> &GetNormalTexture() const { return _normalTexture; }
        void SetNormalTexture(std::shared_ptr<Texture> texture);
        /// How strongly the normal map applies: its x and y are multiplied by it (glTF's normalTexture.scale), so 0
        /// is a flat surface and 1 (the default) the map as authored; a negative one flips x and y (glTF allows it).
        /// Clamped to -4..4.
        [[nodiscard]] float GetNormalScale() const { return _normalScale; }
        void SetNormalScale(float scale);
        /// Pbr only: green is the roughness and blue the metallic factor (glTF's packing), multiplying the
        /// smoothness-derived roughness and metallic. Data, never decoded as sRGB.
        [[nodiscard]] const std::shared_ptr<Texture> &GetMetallicRoughnessTexture() const { return _metallicRoughnessTexture; }
        void SetMetallicRoughnessTexture(std::shared_ptr<Texture> texture);

        /// Blend: drawn in the Transparent queue, blended
        [[nodiscard]] bool IsBlended() const { return _alphaMode == AlphaMode::Blend; }
        /// Goes up by one on every change; 1 for a new material, made at runtime or loaded
        [[nodiscard]] std::uint64_t GetVersion() const { return _version; }
        /// Goes up by one when the shading or the base colour texture changes, what a GPU material is made with (the
        /// GpuCache keys GPU materials by it); 1 for a new material
        [[nodiscard]] std::uint64_t GetGpuVersion() const { return _gpuVersion; }
        /// The file this material was loaded from (empty for one made at runtime)
        [[nodiscard]] const std::string &GetSourcePath() const { return _sourcePath; }

        /**
         * Sets the standard shaders' uniforms on a GPU material from this material: uAlbedo (the base colour times
         * `tint`), uHasTexture (whether `target` has a texture), uAlphaCutoff (the cutoff for Mask, else 0) and,
         * lit, uSmoothness and uMetallic, and the emissive and occlusion inputs: uEmissive (rgb),
         * uHasEmissiveTexture and uHasOcclusionTexture (whether `target` has those textures), and uOcclusionStrength,
         * and the normal map and PBR inputs: uHasNormalTexture, uNormalScale, uPbr (1 for the Pbr shading) and
         * uHasMetallicRoughnessTexture.
         * The drawing code calls it before every draw, so a GPU material shared
         * by several users (or given a per-draw tint) always draws with the right values.
         */
        void ApplyUniforms(Renderer::Common::IMaterial &target, const Common::Color &tint = Common::Color::White) const;

    private:
        /// Bumps the version, and the GPU version too for a change to the shader or texture
        void Changed(const bool structural = false)
        {
            ++_version;
            if (structural)
            {
                ++_gpuVersion;
            }
        }
        /// Reads the keys of a .mat object into this material (see FromJson)
        void ReadJson(const nlohmann::json &json, const std::filesystem::path &baseDirectory, std::string_view debugName);

        ShadingModel _shading = ShadingModel::Lit;
        Common::Color _baseColor{Common::Color::White};
        std::shared_ptr<Texture> _baseColorTexture;
        AlphaMode _alphaMode = AlphaMode::Opaque;
        float _alphaCutoff = 0.5f;
        bool _doubleSided = false;
        float _smoothness = 0.5f;
        float _metallic = 0.0f;
        Common::Color _emissive{Common::Color::Black};
        std::shared_ptr<Texture> _normalTexture;
        float _normalScale = 1.0f;
        std::shared_ptr<Texture> _occlusionTexture;
        float _occlusionStrength = 1.0f;
        std::shared_ptr<Texture> _metallicRoughnessTexture;
        std::shared_ptr<Texture> _emissiveTexture;
        std::uint64_t _version = 1;
        std::uint64_t _gpuVersion = 1;
        std::string _sourcePath;
    };

    /// The loader registered for .mat
    std::shared_ptr<Base::Asset> LoadMaterialFromFile(const std::filesystem::path &path);
}
