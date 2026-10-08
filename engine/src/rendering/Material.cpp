#include "engine/rendering/Material.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <math/UUID.hpp>
#include <renderer/common/IMaterial.hpp>

#include "engine/Logger.hpp"
#include "engine/io/Resources.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        // Lives here because anything that uses Material links this file. RegisterLoader is public too, and
        // ResourceLoader::Initialize calls it, for programs that never name Material.
        struct MaterialLoaderRegistrar
        {
            MaterialLoaderRegistrar()
            {
                Material::RegisterLoader();
            }
        } g_materialLoaderRegistrar;

        // Fixed, like the built-in meshes' (never saved: an empty slot means the default)
        constexpr float kMaxNormalScale = 4.0f; // the scale is clamped to -4..4 (a negative one flips the map's x and y)

        constexpr const char *DefaultLitUuid = "6e32656e-6d61-5454-0001-000000000001";
        constexpr const char *DefaultUnlitUuid = "6e32656e-6d61-5454-0001-000000000002";

        void Warn(const std::string_view debugName, const std::string_view key, const nlohmann::json &value,
                  const std::string_view expected)
        {
            Logger::Warn(std::format("Material {}: ignoring \"{}\": {} (expected {})", debugName, key, value.dump(),
                                     expected));
        }

        std::string Lowercase(std::string text)
        {
            std::ranges::transform(text, text.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::optional<float> ReadUnitFloat(const nlohmann::json &value, const std::string_view debugName,
                                           const std::string_view key)
        {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
            {
                Warn(debugName, key, value, "a number from 0 to 1");
                return std::nullopt;
            }
            const double number = value.get<double>();
            if (number < 0.0 || number > 1.0)
            {
                Warn(debugName, key, value, "a number from 0 to 1 (clamped)");
            }
            return static_cast<float>(std::clamp(number, 0.0, 1.0));
        }

        std::optional<Common::Color> ReadColor(const nlohmann::json &value, const std::string_view debugName,
                                               const std::string_view key)
        {
            const auto allNumbers = [](const nlohmann::json &array)
            {
                for (std::size_t i = 0; i < array.size(); ++i)
                {
                    if (!array[i].is_number())
                    {
                        return false;
                    }
                }
                return true;
            };
            // {"r": .., "g": .., "b": .., "a": ..} as scene files write colours (missing channels are 1), or
            // [r, g, b] / [r, g, b, a]
            if (value.is_object())
            {
                for (const auto &[channel, item] : value.items())
                {
                    if ((channel != "r" && channel != "g" && channel != "b" && channel != "a") || !item.is_number())
                    {
                        Warn(debugName, key, value, "{\"r\", \"g\", \"b\", \"a\"} numbers");
                        return std::nullopt;
                    }
                }
                Common::Color color{Common::Color::White};
                Common::from_json(value, color);
                return color;
            }
            if (value.is_array() && (value.size() == 3 || value.size() == 4) && allNumbers(value))
            {
                return Common::Color{value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
                                     value.size() == 4 ? value[3].get<float>() : 1.0f};
            }
            Warn(debugName, key, value, "a colour: {\"r\", \"g\", \"b\", \"a\"} or [r, g, b(, a)]");
            return std::nullopt;
        }

        /// A texture reference in a .mat file: an asset UUID, a res:// or user:// path, or a path relative to the
        /// .mat file's folder. Null clears it. nullopt (keep the default) after a warning when it can't be used.
        std::optional<std::shared_ptr<Texture>> ReadTexture(const nlohmann::json &value,
                                                            const std::filesystem::path &baseDirectory,
                                                            const std::string_view debugName, const std::string_view key)
        {
            if (value.is_null())
            {
                return std::shared_ptr<Texture>{};
            }
            if (!value.is_string() || value.get<std::string>().empty())
            {
                Warn(debugName, key, value, "a texture path, an asset UUID or null");
                return std::nullopt;
            }
            const std::string spelled = value.get<std::string>();
            IO::Resources &resources = IO::Resources::Instance();
            std::shared_ptr<Texture> texture;
            if (const auto uuid = Math::UUID::FromString(spelled))
            {
                texture = resources.LoadByUUID<Texture>(*uuid);
            }
            else if (spelled.starts_with("res://") || spelled.starts_with("user://") || baseDirectory.empty())
            {
                texture = resources.Load<Texture>(std::filesystem::path(spelled));
            }
            else
            {
                texture = resources.Load<Texture>(baseDirectory / std::filesystem::path(spelled));
            }
            if (!texture || !texture->IsLoaded())
            {
                Logger::Warn(std::format("Material {}: \"{}\": can't load texture '{}'", debugName, key, spelled));
                return std::nullopt;
            }
            return texture;
        }

        nlohmann::json TextureJson(const std::shared_ptr<Texture> &texture)
        {
            if (!texture)
            {
                return nullptr;
            }
            // A model's texture: its resource path is the model's file, so it is saved by its (deterministic) UUID
            if (texture->IsSubResource())
            {
                return texture->GetUUID().ToString();
            }
            if (texture->GetResourcePath().IsValid())
            {
                return texture->GetResourcePath().ToString();
            }
            if (!texture->GetSourcePath().empty())
            {
                return texture->GetSourcePath();
            }
            return texture->GetUUID().ToString();
        }

        nlohmann::json ColorJson(const Common::Color &color)
        {
            nlohmann::json json;
            Common::to_json(json, color);
            return json;
        }

        std::shared_ptr<Material> MakeFixedDefault(const ShadingModel shading, const char *uuid)
        {
            auto material = Material::Create(shading);
            material->SetUUID(Math::UUID::FromString(uuid).value_or(Math::UUID::ZERO));
            return material;
        }
    }

    std::shared_ptr<Material> Material::Create(const ShadingModel shading)
    {
        auto material = std::make_shared<Material>();
        material->_shading = shading;
        return material;
    }

    std::shared_ptr<const Material> Material::GetDefault()
    {
        // Leaked on purpose, like the built-in meshes: a component kept alive by Lua can be destroyed after
        // function-local statics
        static const auto *material = new std::shared_ptr<const Material>(MakeFixedDefault(ShadingModel::Lit, DefaultLitUuid));
        return *material;
    }

    std::shared_ptr<const Material> Material::GetDefaultUnlit()
    {
        static const auto *material =
            new std::shared_ptr<const Material>(MakeFixedDefault(ShadingModel::Unlit, DefaultUnlitUuid));
        return *material;
    }

    void Material::SetShading(const ShadingModel shading)
    {
        _shading = shading;
        Changed(true); // a new shader or texture: a new GPU material
    }

    void Material::SetBaseColor(const Common::Color &color)
    {
        _baseColor = color;
        Changed();
    }

    void Material::SetBaseColorTexture(std::shared_ptr<Texture> texture)
    {
        _baseColorTexture = std::move(texture);
        Changed(true); // a new shader or texture: a new GPU material
    }

    void Material::SetAlphaMode(const AlphaMode mode)
    {
        _alphaMode = mode;
        Changed();
    }

    void Material::SetAlphaCutoff(const float cutoff)
    {
        _alphaCutoff = std::isfinite(cutoff) ? std::clamp(cutoff, 0.0f, 1.0f) : 0.5f;
        Changed();
    }

    void Material::SetDoubleSided(const bool doubleSided)
    {
        _doubleSided = doubleSided;
        Changed();
    }

    void Material::SetSmoothness(const float smoothness)
    {
        _smoothness = std::isfinite(smoothness) ? std::clamp(smoothness, 0.0f, 1.0f) : 0.5f;
        Changed();
    }

    void Material::SetMetallic(const float metallic)
    {
        _metallic = std::isfinite(metallic) ? std::clamp(metallic, 0.0f, 1.0f) : 0.0f;
        Changed();
    }

    void Material::SetEmissive(const Common::Color &emissive)
    {
        _emissive = emissive;
        Changed();
    }

    void Material::SetNormalTexture(std::shared_ptr<Texture> texture)
    {
        _normalTexture = std::move(texture);
        Changed(true); // a new texture: a new GPU material
    }

    void Material::SetNormalScale(const float scale)
    {
        _normalScale = std::isfinite(scale) ? std::clamp(scale, -kMaxNormalScale, kMaxNormalScale) : 1.0f;
        Changed();
    }

    void Material::SetOcclusionTexture(std::shared_ptr<Texture> texture)
    {
        _occlusionTexture = std::move(texture);
        Changed(true); // a new texture: a new GPU material
    }

    void Material::SetOcclusionStrength(const float strength)
    {
        _occlusionStrength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 1.0f;
        Changed();
    }

    void Material::SetMetallicRoughnessTexture(std::shared_ptr<Texture> texture)
    {
        _metallicRoughnessTexture = std::move(texture);
        Changed(true); // a new texture: a new GPU material
    }

    void Material::SetEmissiveTexture(std::shared_ptr<Texture> texture)
    {
        _emissiveTexture = std::move(texture);
        Changed(true); // a new texture: a new GPU material
    }

    void Material::ApplyUniforms(Renderer::Common::IMaterial &target, const Common::Color &tint) const
    {
        target.SetColor("uAlbedo", _baseColor.r * tint.r, _baseColor.g * tint.g, _baseColor.b * tint.b,
                        _baseColor.a * tint.a);
        target.SetInt("uHasTexture", target.GetTexture() != nullptr ? 1 : 0);
        target.SetFloat("uAlphaCutoff", _alphaMode == AlphaMode::Mask ? _alphaCutoff : 0.0f);
        if (_shading != ShadingModel::Unlit)
        {
            target.SetFloat("uSmoothness", _smoothness);
            target.SetFloat("uMetallic", _metallic);
            target.SetInt("uPbr", _shading == ShadingModel::Pbr ? 1 : 0);
            target.SetInt("uHasNormalTexture", target.GetAuxTexture(Renderer::Common::AuxTexture::Normal) != nullptr ? 1 : 0);
            target.SetFloat("uNormalScale", _normalScale);
            // Only the Pbr shading reads it (and GpuCache only gives it to a Pbr material)
            target.SetInt("uHasMetallicRoughnessTexture",
                          _shading == ShadingModel::Pbr &&
                                  target.GetAuxTexture(Renderer::Common::AuxTexture::MetallicRoughness) != nullptr
                              ? 1
                              : 0);
            target.SetVec3("uEmissive", _emissive.r, _emissive.g, _emissive.b);
            target.SetInt("uHasEmissiveTexture", target.GetAuxTexture(Renderer::Common::AuxTexture::Emissive) != nullptr ? 1 : 0);
            target.SetInt("uHasOcclusionTexture", target.GetAuxTexture(Renderer::Common::AuxTexture::Occlusion) != nullptr ? 1 : 0);
            target.SetFloat("uOcclusionStrength", _occlusionStrength);
        }
    }

    // ===== .mat files =====

    std::shared_ptr<Material> Material::FromJson(const nlohmann::json &json, const std::filesystem::path &baseDirectory,
                                                 const std::string_view debugName)
    {
        if (!json.is_object())
        {
            Logger::Error(std::format("Material {}: a .mat file must hold a JSON object", debugName));
            return nullptr;
        }
        auto material = std::make_shared<Material>();
        material->ReadJson(json, baseDirectory, debugName);
        return material;
    }

    void Material::ReadJson(const nlohmann::json &json, const std::filesystem::path &baseDirectory,
                            const std::string_view debugName)
    {
        for (const auto &[key, value] : json.items())
        {
            if (key == "shading")
            {
                const std::string name = value.is_string() ? Lowercase(value.get<std::string>()) : std::string{};
                if (name == "lit")
                    _shading = ShadingModel::Lit;
                else if (name == "unlit")
                    _shading = ShadingModel::Unlit;
                else if (name == "pbr")
                    _shading = ShadingModel::Pbr;
                else
                    Warn(debugName, key, value, "\"lit\", \"unlit\" or \"pbr\"");
            }
            else if (key == "baseColor")
            {
                if (const auto color = ReadColor(value, debugName, key))
                    _baseColor = *color;
            }
            else if (key == "baseColorTexture")
            {
                if (auto texture = ReadTexture(value, baseDirectory, debugName, key))
                    _baseColorTexture = std::move(*texture);
            }
            else if (key == "alphaMode")
            {
                // glTF spells them in capitals; either case is accepted
                const std::string name = value.is_string() ? Lowercase(value.get<std::string>()) : std::string{};
                if (name == "opaque")
                    _alphaMode = AlphaMode::Opaque;
                else if (name == "mask")
                    _alphaMode = AlphaMode::Mask;
                else if (name == "blend")
                    _alphaMode = AlphaMode::Blend;
                else
                    Warn(debugName, key, value, "\"opaque\", \"mask\" or \"blend\"");
            }
            else if (key == "alphaCutoff")
            {
                if (const auto number = ReadUnitFloat(value, debugName, key))
                    _alphaCutoff = *number;
            }
            else if (key == "doubleSided")
            {
                if (value.is_boolean())
                    _doubleSided = value.get<bool>();
                else
                    Warn(debugName, key, value, "true or false");
            }
            else if (key == "smoothness")
            {
                if (const auto number = ReadUnitFloat(value, debugName, key))
                    _smoothness = *number;
            }
            else if (key == "metallic")
            {
                if (const auto number = ReadUnitFloat(value, debugName, key))
                    _metallic = *number;
            }
            else if (key == "emissive")
            {
                if (const auto color = ReadColor(value, debugName, key))
                    _emissive = *color;
            }
            else if (key == "normalTexture")
            {
                if (auto texture = ReadTexture(value, baseDirectory, debugName, key))
                    _normalTexture = std::move(*texture);
            }
            else if (key == "normalScale")
            {
                if (value.is_number() && std::isfinite(value.get<double>()))
                {
                    const double number = value.get<double>();
                    if (std::fabs(number) > static_cast<double>(kMaxNormalScale))
                        Warn(debugName, key, value, "a number from -4 to 4 (clamped)");
                    _normalScale = static_cast<float>(
                        std::clamp(number, -static_cast<double>(kMaxNormalScale), static_cast<double>(kMaxNormalScale)));
                }
                else
                {
                    Warn(debugName, key, value, "a number from -4 to 4");
                }
            }
            else if (key == "occlusionTexture")
            {
                if (auto texture = ReadTexture(value, baseDirectory, debugName, key))
                    _occlusionTexture = std::move(*texture);
            }
            else if (key == "occlusionStrength")
            {
                if (const auto number = ReadUnitFloat(value, debugName, key))
                    _occlusionStrength = *number;
            }
            else if (key == "metallicRoughnessTexture")
            {
                if (auto texture = ReadTexture(value, baseDirectory, debugName, key))
                    _metallicRoughnessTexture = std::move(*texture);
            }
            else if (key == "emissiveTexture")
            {
                if (auto texture = ReadTexture(value, baseDirectory, debugName, key))
                    _emissiveTexture = std::move(*texture);
            }
            else
            {
                Logger::Warn(std::format("Material {}: ignoring unknown key \"{}\"", debugName, key));
            }
        }
        // Not a change: only a new material reads JSON, so a loaded one starts at version 1 like any other
    }

    nlohmann::json Material::ToJson() const
    {
        nlohmann::json json;
        switch (_shading)
        {
        case ShadingModel::Unlit:
            json["shading"] = "unlit";
            break;
        case ShadingModel::Lit:
            json["shading"] = "lit";
            break;
        case ShadingModel::Pbr:
            json["shading"] = "pbr";
            break;
        }
        json["baseColor"] = ColorJson(_baseColor);
        json["baseColorTexture"] = TextureJson(_baseColorTexture);
        switch (_alphaMode)
        {
        case AlphaMode::Opaque:
            json["alphaMode"] = "opaque";
            break;
        case AlphaMode::Mask:
            json["alphaMode"] = "mask";
            break;
        case AlphaMode::Blend:
            json["alphaMode"] = "blend";
            break;
        }
        json["alphaCutoff"] = _alphaCutoff;
        json["doubleSided"] = _doubleSided;
        json["smoothness"] = _smoothness;
        json["metallic"] = _metallic;
        json["emissive"] = ColorJson(_emissive);
        json["normalTexture"] = TextureJson(_normalTexture);
        json["normalScale"] = _normalScale;
        json["occlusionTexture"] = TextureJson(_occlusionTexture);
        json["occlusionStrength"] = _occlusionStrength;
        json["metallicRoughnessTexture"] = TextureJson(_metallicRoughnessTexture);
        json["emissiveTexture"] = TextureJson(_emissiveTexture);
        return json;
    }

    void Material::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            IO::Resources::Instance().RegisterLoader(".mat", LoadMaterialFromFile);
        });
    }

    bool Material::Load(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            Logger::Error(std::format("Cannot read material file {}", path.string()));
            return false;
        }
        nlohmann::json json;
        try
        {
            json = nlohmann::json::parse(file);
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::format("Material {}: not valid JSON: {}", path.string(), e.what()));
            return false;
        }
        if (!json.is_object())
        {
            Logger::Error(std::format("Material {}: a .mat file must hold a JSON object", path.string()));
            return false;
        }
        ReadJson(json, path.parent_path(), path.string());
        _sourcePath = path.string();
        return true;
    }

    std::shared_ptr<Base::Asset> LoadMaterialFromFile(const std::filesystem::path &path)
    {
        auto material = std::make_shared<Material>();
        if (!material->Load(path))
        {
            return nullptr;
        }
        return material;
    }
}
