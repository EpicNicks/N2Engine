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
        Lit    ///< Blinn-Phong with the scene's lights (GetStandardLitShader)
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
     * metallic, emissive and the normal, occlusion, metallic-roughness and emissive textures are stored and
     * serialized but have no effect yet: physically based shading arrives with #3 P4.
     *
     * Materials come from `.mat` files (JSON, see Load) or Create at runtime. Every change bumps GetVersion, and
     * a renderer then makes a new GPU material for the new version on its next draw.
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
         *    "normalTexture": null, "occlusionTexture": null, "metallicRoughnessTexture": null,
         *    "emissiveTexture": null}
         * See FromJson for how keys are read. False, with an error, if the file can't be read or isn't a JSON
         * object.
         */
        bool Load(const std::filesystem::path &path) override;
        [[nodiscard]] std::string GetResourceType() const override { return "Material"; }

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
        /// Lit only: 0 (dull, wide highlights) to 1 (glossy, tight highlights); 0.5 by default
        [[nodiscard]] float GetSmoothness() const { return _smoothness; }
        void SetSmoothness(float smoothness);

        // Stored and serialized, no effect until #3 P4
        [[nodiscard]] float GetMetallic() const { return _metallic; }
        void SetMetallic(float metallic);
        [[nodiscard]] const Common::Color &GetEmissive() const { return _emissive; }
        void SetEmissive(const Common::Color &emissive);
        [[nodiscard]] const std::shared_ptr<Texture> &GetNormalTexture() const { return _normalTexture; }
        void SetNormalTexture(std::shared_ptr<Texture> texture);
        [[nodiscard]] const std::shared_ptr<Texture> &GetOcclusionTexture() const { return _occlusionTexture; }
        void SetOcclusionTexture(std::shared_ptr<Texture> texture);
        [[nodiscard]] const std::shared_ptr<Texture> &GetMetallicRoughnessTexture() const { return _metallicRoughnessTexture; }
        void SetMetallicRoughnessTexture(std::shared_ptr<Texture> texture);
        [[nodiscard]] const std::shared_ptr<Texture> &GetEmissiveTexture() const { return _emissiveTexture; }
        void SetEmissiveTexture(std::shared_ptr<Texture> texture);

        /// Blend: drawn in the Transparent queue, blended
        [[nodiscard]] bool IsBlended() const { return _alphaMode == AlphaMode::Blend; }
        /// Goes up by one on every change; 1 for a new material
        [[nodiscard]] std::uint64_t GetVersion() const { return _version; }
        /// The file this material was loaded from (empty for one made at runtime)
        [[nodiscard]] const std::string &GetSourcePath() const { return _sourcePath; }

        /**
         * Sets the standard shaders' uniforms on a GPU material from this material: uAlbedo (the base colour times
         * `tint`), uHasTexture (whether `target` has a texture), uAlphaCutoff (the cutoff for Mask, else 0) and,
         * lit, uSmoothness and uMetallic. The drawing code calls it before every draw, so a GPU material shared
         * by several users (or given a per-draw tint) always draws with the right values.
         */
        void ApplyUniforms(Renderer::Common::IMaterial &target, const Common::Color &tint = Common::Color::White) const;

    private:
        void Changed() { ++_version; }
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
        std::shared_ptr<Texture> _occlusionTexture;
        std::shared_ptr<Texture> _metallicRoughnessTexture;
        std::shared_ptr<Texture> _emissiveTexture;
        std::uint64_t _version = 1;
        std::string _sourcePath;
    };

    /// The loader registered for .mat
    std::shared_ptr<Base::Asset> LoadMaterialFromFile(const std::filesystem::path &path);
}
