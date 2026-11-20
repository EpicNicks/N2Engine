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
#include <renderer/common/ColorEffects.hpp>
#include <renderer/common/IMaterial.hpp>

#include "engine/Logger.hpp"
#include "engine/io/Resources.hpp"
#include "engine/io/Vfs.hpp"
#include "engine/rendering/RenderSettings.hpp"
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

        /// A shader reference in a .mat file: an asset UUID, a res:// or user:// path, or a path relative to the .mat file's folder. Null
        /// clears it. nullopt (keep what it was) after a warning when it can't be used. Read as ReadTexture reads a texture
        std::optional<std::shared_ptr<Shader>> ReadShader(const nlohmann::json &value, const std::filesystem::path &baseDirectory,
                                                          const std::string_view debugName, const std::string_view key)
        {
            if (value.is_null())
            {
                return std::shared_ptr<Shader>{};
            }
            if (!value.is_string() || value.get<std::string>().empty())
            {
                Warn(debugName, key, value, "a .n2shader path, an asset UUID or null");
                return std::nullopt;
            }
            const std::string spelled = value.get<std::string>();
            IO::Resources &resources = IO::Resources::Instance();
            std::shared_ptr<Shader> shader;
            if (const auto uuid = Math::UUID::FromString(spelled))
            {
                shader = resources.LoadByUUID<Shader>(*uuid);
            }
            else if (spelled.starts_with("res://") || spelled.starts_with("user://") || baseDirectory.empty())
            {
                shader = resources.Load<Shader>(std::filesystem::path(spelled));
            }
            else
            {
                shader = resources.Load<Shader>(baseDirectory / std::filesystem::path(spelled));
            }
            if (!shader)
            {
                Logger::Warn(std::format("Material {}: \"{}\": can't load shader '{}'", debugName, key, spelled));
                return std::nullopt;
            }
            return shader;
        }

        nlohmann::json ShaderJson(const std::shared_ptr<Shader> &shader)
        {
            if (shader->GetResourcePath().IsValid())
            {
                return shader->GetResourcePath().ToString();
            }
            if (!shader->GetSourcePath().empty())
            {
                return shader->GetSourcePath();
            }
            return shader->GetUUID().ToString();
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

    std::shared_ptr<Material> Material::Clone() const
    {
        auto copy = std::make_shared<Material>(*this);
        copy->_warnedParams.clear();
        return copy;
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

    void Material::SetShader(std::shared_ptr<Shader> shader)
    {
        if (_shader != shader)
        {
            _params.clear(); // the old shader's names and types mean nothing to the new one
            _warnedParams.clear();
        }
        _shader = std::move(shader);
        Changed(true); // a different shader: a new GPU material
    }

    const ShaderParam *Material::MatchParam(const std::string_view name, const std::initializer_list<ShaderParamType> accepted,
                                            const char *setter)
    {
        const ShaderParam *param = _shader ? _shader->FindParam(name) : nullptr;
        const bool fits = param != nullptr && std::ranges::find(accepted, param->type) != accepted.end();
        if (fits)
        {
            return param;
        }
        if (_warnedParams.insert(std::string(setter) + ":" + std::string(name)).second)
        {
            Logger::Warn(!_shader ? std::format("Material {}: {}(\"{}\") ignored: the material has no shader", _sourcePath, setter, name)
                         : !param ? std::format("Material {}: {}(\"{}\") ignored: shader '{}' declares no such parameter", _sourcePath, setter,
                                                name, _shader->GetName())
                                  : std::format("Material {}: {}(\"{}\") ignored: the parameter's type does not fit this setter", _sourcePath,
                                                setter, name));
        }
        return nullptr;
    }

    namespace
    {
        float Limited(const ShaderParam &param, float value)
        {
            if (!std::isfinite(value))
            {
                return param.defaultValue[0];
            }
            if (param.min)
            {
                value = std::max(value, *param.min);
            }
            if (param.max)
            {
                value = std::min(value, *param.max);
            }
            return value;
        }
    }

    bool Material::SetFloat(const std::string_view name, const float value)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Float}, "SetFloat");
        if (!param)
        {
            return false;
        }
        _params[param->name].value[0] = Limited(*param, value);
        Changed();
        return true;
    }

    bool Material::SetInt(const std::string_view name, const int value)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Int, ShaderParamType::Bool}, "SetInt");
        if (!param)
        {
            return false;
        }
        const float stored = param->type == ShaderParamType::Bool ? (value != 0 ? 1.0f : 0.0f)
                                                                   : std::trunc(Limited(*param, static_cast<float>(value)));
        _params[param->name].value[0] = stored;
        Changed();
        return true;
    }

    bool Material::SetVec2(const std::string_view name, const float x, const float y)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Vec2}, "SetVec2");
        if (!param)
        {
            return false;
        }
        _params[param->name].value = {std::isfinite(x) ? x : 0.0f, std::isfinite(y) ? y : 0.0f, 0.0f, 0.0f};
        Changed();
        return true;
    }

    bool Material::SetVec3(const std::string_view name, const float x, const float y, const float z)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Vec3}, "SetVec3");
        if (!param)
        {
            return false;
        }
        _params[param->name].value = {std::isfinite(x) ? x : 0.0f, std::isfinite(y) ? y : 0.0f, std::isfinite(z) ? z : 0.0f, 0.0f};
        Changed();
        return true;
    }

    bool Material::SetVec4(const std::string_view name, const float x, const float y, const float z, const float w)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Vec4, ShaderParamType::Color}, "SetVec4");
        if (!param)
        {
            return false;
        }
        _params[param->name].value = {std::isfinite(x) ? x : 0.0f, std::isfinite(y) ? y : 0.0f, std::isfinite(z) ? z : 0.0f,
                                      std::isfinite(w) ? w : 0.0f};
        Changed();
        return true;
    }

    bool Material::SetColor(const std::string_view name, const Common::Color &color)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Color, ShaderParamType::Vec4}, "SetColor");
        if (!param)
        {
            return false;
        }
        _params[param->name].value = {color.r, color.g, color.b, color.a};
        Changed();
        return true;
    }

    bool Material::SetTexture(const std::string_view name, std::shared_ptr<Texture> texture)
    {
        const ShaderParam *param = MatchParam(name, {ShaderParamType::Texture}, "SetTexture");
        if (!param)
        {
            return false;
        }
        if (texture && !texture->IsLoaded())
        {
            Logger::Warn(std::format("Material {}: SetTexture(\"{}\") ignored: the texture is not loaded", _sourcePath, name));
            return false;
        }
        _params[param->name].texture = std::move(texture);
        Changed(true); // a texture is part of the GPU material
        return true;
    }

    std::optional<MaterialParamValue> Material::GetParam(const std::string_view name) const
    {
        const ShaderParam *param = _shader ? _shader->FindParam(name) : nullptr;
        if (!param)
        {
            return std::nullopt;
        }
        if (const auto it = _params.find(param->name); it != _params.end())
        {
            return it->second;
        }
        MaterialParamValue value;
        value.value = param->defaultValue;
        return value;
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

    void Material::SetBlendDiscardBelow(const float alpha)
    {
        _blendDiscardBelow = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f;
        Changed();
    }

    void Material::SetSaturation(const float saturation)
    {
        _saturation = Renderer::Common::ColorEffects::ClampSaturation(saturation);
        Changed();
    }

    void Material::SetFlash(const Common::Color &flash)
    {
        _flash = Renderer::Common::ColorEffects::ClampFlash(flash);
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

    void Material::ApplyShaderParams(Renderer::Common::IMaterial &target, const Common::Color &tint) const
    {
        for (const ShaderParam &param : _shader->GetParams())
        {
            if (param.type == ShaderParamType::Texture)
            {
                continue; // bound when the GPU material is made (GpuCache::AcquireMaterial), by the slot the cook gave it
            }
            const auto set = _params.find(param.name);
            std::array<float, 4> v = set != _params.end() ? set->second.value : param.defaultValue;
            if (param.type == ShaderParamType::Color && param.name == "baseColor")
            {
                // The reserved name: the material's own base colour unless the .mat sets one, times the draw's tint
                if (set == _params.end())
                {
                    v = {_baseColor.r, _baseColor.g, _baseColor.b, _baseColor.a};
                }
                v = {v[0] * tint.r, v[1] * tint.g, v[2] * tint.b, v[3] * tint.a};
            }
            switch (param.type)
            {
            case ShaderParamType::Float: target.SetFloat(param.name, v[0]); break;
            case ShaderParamType::Int:
            case ShaderParamType::Bool: target.SetInt(param.name, static_cast<int>(v[0])); break;
            case ShaderParamType::Vec2: target.SetVec2(param.name, v[0], v[1]); break;
            case ShaderParamType::Vec3: target.SetVec3(param.name, v[0], v[1], v[2]); break;
            case ShaderParamType::Vec4: target.SetVec4(param.name, v[0], v[1], v[2], v[3]); break;
            case ShaderParamType::Color: target.SetColor(param.name, v[0], v[1], v[2], v[3]); break;
            case ShaderParamType::Texture: break;
            }
        }
    }

    void Material::ApplyUniforms(Renderer::Common::IMaterial &target, const Common::Color &tint) const
    {
        if (_shader && target.IsUserShaderMaterial())
        {
            ApplyShaderParams(target, tint);
            return;
        }
        // A material with a shader whose GPU material is a standard one: its shader could not be drawn (GpuCache gave it the unlit
        // fallback), so it is the unlit uniforms, and magenta where the host wants the failure to show
        const bool fallback = _shader != nullptr;
        const ShadingModel shading = fallback ? ShadingModel::Unlit : _shading;
        if (fallback && RenderSettings::GetUserShaderErrorStyle() == UserShaderErrorStyle::Magenta)
        {
            target.SetColor("uAlbedo", 1.0f, 0.0f, 1.0f, 1.0f);
        }
        else
        {
            target.SetColor("uAlbedo", _baseColor.r * tint.r, _baseColor.g * tint.g, _baseColor.b * tint.b,
                            _baseColor.a * tint.a);
        }
        target.SetInt("uHasTexture", target.GetTexture() != nullptr ? 1 : 0);
        float cutoff = 0.0f;
        if (_alphaMode == AlphaMode::Mask)
        {
            cutoff = _alphaCutoff;
        }
        else if (_alphaMode == AlphaMode::Blend)
        {
            cutoff = _blendDiscardBelow;
        }
        target.SetFloat("uAlphaCutoff", cutoff);
        if (shading == ShadingModel::Unlit)
        {
            target.SetFloat("uSaturation", _saturation);
            target.SetVec4("uFlash", _flash.r, _flash.g, _flash.b, _flash.a);
        }
        else
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
        // "params" depends on "shader" (the shader says which names and types exist), and a JSON object's keys come out sorted, so it is
        // applied after the loop
        const nlohmann::json *paramsJson = nullptr;
        for (const auto &[key, value] : json.items())
        {
            if (key == "shader")
            {
                if (auto shader = ReadShader(value, baseDirectory, debugName, key))
                    _shader = std::move(*shader);
            }
            else if (key == "params")
            {
                paramsJson = &value;
            }
            else if (key == "shading")
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

        if (_shader)
        {
            // The shader's state hints are the defaults of what the .mat does not say (a value it sets wins)
            const ShaderStateHints &hints = _shader->GetStateHints();
            if (!json.contains("alphaMode") && hints.blend && (*hints.blend == "alpha" || *hints.blend == "additive"))
                _alphaMode = AlphaMode::Blend;
            if (!json.contains("doubleSided") && hints.cull && *hints.cull == "none")
                _doubleSided = true;
        }
        if (paramsJson)
        {
            if (!_shader)
            {
                Warn(debugName, "params", *paramsJson, "a shader to hold them (the \"shader\" key)");
            }
            else if (!paramsJson->is_object())
            {
                Warn(debugName, "params", *paramsJson, "an object of parameter values");
            }
            else
            {
                for (const auto &[name, value] : paramsJson->items())
                {
                    const ShaderParam *param = _shader->FindParam(name);
                    if (!param)
                    {
                        Logger::Warn(std::format("Material {}: params: ignoring \"{}\": shader '{}' declares no such parameter",
                                                 debugName, name, _shader->GetName()));
                        continue;
                    }
                    MaterialParamValue stored;
                    bool ok = false;
                    switch (param->type)
                    {
                    case ShaderParamType::Float:
                    case ShaderParamType::Int:
                    case ShaderParamType::Bool:
                        if (param->type == ShaderParamType::Bool ? value.is_boolean() : (value.is_number() && std::isfinite(value.get<double>())))
                        {
                            float number = param->type == ShaderParamType::Bool ? (value.get<bool>() ? 1.0f : 0.0f)
                                                                                : static_cast<float>(value.get<double>());
                            if (param->type == ShaderParamType::Int)
                                number = std::trunc(number);
                            if (param->type != ShaderParamType::Bool)
                            {
                                if (param->min)
                                    number = std::max(number, *param->min);
                                if (param->max)
                                    number = std::min(number, *param->max);
                            }
                            stored.value[0] = number;
                            ok = true;
                        }
                        else
                        {
                            Warn(debugName, name, value, param->type == ShaderParamType::Bool ? "true or false" : "a number");
                        }
                        break;
                    case ShaderParamType::Vec2:
                    case ShaderParamType::Vec3:
                    case ShaderParamType::Vec4:
                    {
                        const std::size_t count = param->type == ShaderParamType::Vec2 ? 2 : param->type == ShaderParamType::Vec3 ? 3 : 4;
                        bool numbers = value.is_array() && value.size() == count;
                        for (std::size_t i = 0; numbers && i < count; ++i)
                            numbers = value[i].is_number() && std::isfinite(value[i].get<double>());
                        if (numbers)
                        {
                            for (std::size_t i = 0; i < count; ++i)
                                stored.value[i] = value[i].get<float>();
                            ok = true;
                        }
                        else
                        {
                            Warn(debugName, name, value, std::format("an array of {} numbers", count));
                        }
                        break;
                    }
                    case ShaderParamType::Color:
                        if (const auto color = ReadColor(value, debugName, name))
                        {
                            stored.value = {color->r, color->g, color->b, color->a};
                            ok = true;
                        }
                        break;
                    case ShaderParamType::Texture:
                        if (auto texture = ReadTexture(value, baseDirectory, debugName, name))
                        {
                            stored.texture = std::move(*texture);
                            ok = true;
                        }
                        break;
                    }
                    if (ok)
                        _params[param->name] = std::move(stored);
                }
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
        if (_shader)
        {
            // Only a material with a shader writes these two keys: every other .mat file reads and writes exactly what it always did
            json["shader"] = ShaderJson(_shader);
            nlohmann::json params = nlohmann::json::object();
            for (const auto &[name, stored] : _params)
            {
                const ShaderParam *param = _shader->FindParam(name);
                if (!param)
                    continue;
                switch (param->type)
                {
                case ShaderParamType::Float: params[name] = stored.value[0]; break;
                case ShaderParamType::Int: params[name] = static_cast<int>(stored.value[0]); break;
                case ShaderParamType::Bool: params[name] = stored.value[0] != 0.0f; break;
                case ShaderParamType::Vec2: params[name] = nlohmann::json::array({stored.value[0], stored.value[1]}); break;
                case ShaderParamType::Vec3: params[name] = nlohmann::json::array({stored.value[0], stored.value[1], stored.value[2]}); break;
                case ShaderParamType::Vec4:
                case ShaderParamType::Color:
                    params[name] = nlohmann::json::array({stored.value[0], stored.value[1], stored.value[2], stored.value[3]});
                    break;
                case ShaderParamType::Texture: params[name] = TextureJson(stored.texture); break;
                }
            }
            json["params"] = std::move(params);
        }
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
        // Through the virtual filesystem: a folder, or a pack (docs/packs.html)
        const auto text = IO::Vfs::Instance().ReadText(path);
        if (!text)
        {
            Logger::Error(std::format("Cannot read material file {}", path.string()));
            return false;
        }
        nlohmann::json json;
        try
        {
            json = nlohmann::json::parse(*text);
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
