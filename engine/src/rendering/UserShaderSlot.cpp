#include "engine/rendering/UserShaderSlot.hpp"

#include <utility>

namespace N2Engine::Rendering
{
    void UserShaderSlot::SetShader(std::shared_ptr<Shader> shader)
    {
        if (shader == _shader)
        {
            return;
        }
        _gpu.Release(true);
        _shader = std::move(shader);
        if (!_shader)
        {
            _material.reset();
            return;
        }
        if (!_material)
        {
            _material = Material::Create(ShadingModel::Unlit);
            _material->SetAlphaMode(AlphaMode::Blend);
            _material->SetDoubleSided(true);
        }
        _material->SetShader(_shader); // forgets the values of the old shader
        // The shader's blend hint (premultiplied, multiply, additive; alpha otherwise) is how the component's draws blend
        BlendMode mode = BlendMode::Alpha;
        if (const auto &hint = _shader->GetStateHints().blend)
        {
            mode = *hint == "additive" ? BlendMode::Additive
                : *hint == "premultiplied" ? BlendMode::Premultiplied
                : *hint == "multiply"      ? BlendMode::Multiply
                                           : BlendMode::Alpha;
        }
        _material->SetBlendMode(mode);
    }

    bool UserShaderSlot::SetFloat(const std::string_view name, const float value)
    {
        return _material && _material->SetFloat(name, value);
    }

    bool UserShaderSlot::SetInt(const std::string_view name, const int value)
    {
        return _material && _material->SetInt(name, value);
    }

    bool UserShaderSlot::SetVec2(const std::string_view name, const float x, const float y)
    {
        return _material && _material->SetVec2(name, x, y);
    }

    bool UserShaderSlot::SetVec3(const std::string_view name, const float x, const float y, const float z)
    {
        return _material && _material->SetVec3(name, x, y, z);
    }

    bool UserShaderSlot::SetVec4(const std::string_view name, const float x, const float y, const float z, const float w)
    {
        return _material && _material->SetVec4(name, x, y, z, w);
    }

    bool UserShaderSlot::SetColor(const std::string_view name, const Common::Color &color)
    {
        return _material && _material->SetColor(name, color);
    }

    bool UserShaderSlot::SetTexture(const std::string_view name, std::shared_ptr<Texture> texture)
    {
        return _material && _material->SetTexture(name, std::move(texture));
    }

    void UserShaderSlot::Sync(const std::shared_ptr<Texture> &baseColorTexture, const float blendDiscardBelow)
    {
        if (!_material)
        {
            return;
        }
        if (_material->GetBaseColorTexture() != baseColorTexture)
        {
            _material->SetBaseColorTexture(baseColorTexture);
        }
        if (_material->GetBlendDiscardBelow() != blendDiscardBelow)
        {
            _material->SetBlendDiscardBelow(blendDiscardBelow);
        }
    }

    Renderer::Common::IMaterial *UserShaderSlot::Prepare(Renderer::Common::IRenderer &renderer, const Common::Color &tint)
    {
        if (!_material)
        {
            return nullptr;
        }
        // A new slot, another renderer, or a Material whose shader or texture changed (uniform-only changes keep the GPU material:
        // ApplyUniforms sets them per draw). The new share is taken before the old one goes.
        if (!_gpu.Holds(&renderer) || _gpu.GetSource() != _material.get() || _gpu.GetVersion() != GpuCache::MaterialVersion(*_material))
        {
            _gpu = GpuCache::AcquireMaterial(renderer, _material);
        }
        Renderer::Common::IMaterial *gpu = _gpu.GetMaterial();
        if (gpu != nullptr)
        {
            _material->ApplyUniforms(*gpu, tint);
        }
        return gpu;
    }
}
