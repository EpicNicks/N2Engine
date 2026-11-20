#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Texture.hpp"

namespace N2Engine::Rendering
{
    /**
     * What a component keeps to draw with a user shader (`.n2shader`, orchestrator/proposals/user-shaders.md, stage 8): the Shader asset and
     * a PRIVATE Material of its own (unlit shading, blended, both faces) that holds the shader and the parameter values, so one object's
     * values never reach another's (the Material is never shared; the slot saves its shader and values itself, Save). Used by Sprite2D, the UI Image and ParticleSystem.
     *
     * Render-only, like the Lua Material instances: the component saves it in a scene or prefab file (`shaderState`, only when a shader is
     * set, so a file without one is what it always was) but it is in no schema and no replicated field, so it is never in a rollback
     * snapshot, a replication payload or a golden-replay hash (rendering is not simulation state). A component without a shader never
     * touches any of this: its draw path is exactly what it was.
     *
     * Where the shader cannot be drawn (the software renderer, a shader that is not cooked or compiled, a compile error) the Material gets
     * the fallback GpuCache gives any Material with a shader: the built-in unlit shader with the base colour and texture (flat magenta in
     * the editor host), so the component still shows where it is. The cut-out of a texture under half alpha on a renderer that cannot
     * blend is kept, as Sprite2D and Image do for the built-in shader. The component's saturation and flash effects are the built-in
     * shaders' uniforms and are not applied by a user shader (it declares what it reads).
     */
    class UserShaderSlot
    {
    public:
        /// The shader to draw with; null goes back to the built-in one and forgets the parameter values. Setting the shader it has
        /// already keeps them
        void SetShader(std::shared_ptr<Shader> shader);
        [[nodiscard]] const std::shared_ptr<Shader> &GetShader() const { return _shader; }
        [[nodiscard]] bool HasShader() const { return _shader != nullptr; }

        /// The private Material (null without a shader): the shader's declared parameters live in it (Material::GetParam)
        [[nodiscard]] const std::shared_ptr<Material> &GetMaterial() const { return _material; }

        /// Material's typed setters, on the private Material. False, with one warning per name, without a shader, for a name the
        /// shader does not declare or a value that does not fit its type
        bool SetFloat(std::string_view name, float value);
        bool SetInt(std::string_view name, int value);
        bool SetVec2(std::string_view name, float x, float y);
        bool SetVec3(std::string_view name, float x, float y, float z);
        bool SetVec4(std::string_view name, float x, float y, float z, float w);
        bool SetColor(std::string_view name, const Common::Color &color);
        bool SetTexture(std::string_view name, std::shared_ptr<Texture> texture);

        /// The same, by the value's type (what a component's SetShaderParam forwards to): a float, a vec2, a vec3, a colour (a vec4 or a
        /// color parameter) or a texture; SetParamInt for an int or a bool parameter, SetParamVec4 for a vec4 given as four numbers
        bool SetParam(std::string_view name, float value) { return SetFloat(name, value); }
        bool SetParam(std::string_view name, const Math::Vector2 &value) { return SetVec2(name, value.x, value.y); }
        bool SetParam(std::string_view name, const Math::Vector3 &value) { return SetVec3(name, value.x, value.y, value.z); }
        bool SetParam(std::string_view name, const Common::Color &value) { return SetColor(name, value); }
        bool SetParam(std::string_view name, std::shared_ptr<Texture> value) { return SetTexture(name, std::move(value)); }
        bool SetParamInt(std::string_view name, int value) { return SetInt(name, value); }
        bool SetParamVec4(std::string_view name, float x, float y, float z, float w) { return SetVec4(name, x, y, z, w); }

        /// The slot as a scene or prefab file keeps it: `{"shader": <the asset's UUID>, "params": {the values set, as a .mat writes them}}`, or
        /// null without a shader (the component then writes nothing, so a file without a shader is what it always was). The shader is named by
        /// UUID, as every asset reference in a scene is, so it survives a move of the asset. Each instance holds its own values, so a
        /// prefab's instances differ the way any component fields do. Still runtime-only for the simulation: not a replicated field, so no
        /// snapshot, replication payload or replay hash holds it
        [[nodiscard]] nlohmann::json Save() const;
        /// Reads what Save wrote; null, anything else or a shader that cannot be loaded (warned about) clears the slot
        void Load(const nlohmann::json &json);

        /// Gives the Material the draw's inputs: the component's texture as the base colour texture (the shader's reserved
        /// baseColorTexture) and the cut-out for a renderer that cannot blend. Cheap: each is set only when it changed
        void Sync(const std::shared_ptr<Texture> &baseColorTexture, float blendDiscardBelow);

        /// For a component that draws a raw mesh of its own (Image, ParticleSystem): the GPU material on `renderer`, made or kept for the
        /// Material's current version, with its uniforms set from the Material and `tint` (the draw's colour, which reaches the shader's
        /// reserved baseColor parameter). Null when the renderer cannot make one. Call Sync first
        [[nodiscard]] Renderer::Common::IMaterial *Prepare(Renderer::Common::IRenderer &renderer, const Common::Color &tint);

        /// Gives up the GPU material share (callRenderer false when the renderer may be gone)
        void Release(bool callRenderer) { _gpu.Release(callRenderer); }

    private:
        std::shared_ptr<Shader> _shader;
        std::shared_ptr<Material> _material;
        GpuCache::Handle _gpu;
    };
}
