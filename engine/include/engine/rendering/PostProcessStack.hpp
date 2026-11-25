#pragma once

#include <string>

#include <renderer/common/PostProcess.hpp>

#include "engine/common/Color.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Rendering
{
    /**
     * The scene's full-screen effects (issue #445; docs/post-processing.html): bloom, colour inversion, chromatic aberration
     * and a vignette, run by the Application over the scene pass of every frame, before the screen-space UI (so the HUD is not
     * bloomed, as in Unity). Put one on any object of the scene: the first active one the scene finds is used, and a scene with
     * none (or with every effect off) draws exactly as before, with no extra pass. The fields are Renderer::Common::
     * PostProcessSettings', edited in the inspector and saved with the scene; out-of-range values are clamped when the frame
     * uses them. Lua: the PostProcessStack component, PostProcessStack:Set{...} and :Get().
     *
     * Every renderer applies it (OpenGL, Vulkan and WebGPU on the GPU; the software renderer on the CPU, at a cost of a few
     * full-frame passes). A renderer without post-processing (a test fake) logs once and draws the scene unprocessed.
     */
    class PostProcessStack : public SerializableComponent
    {
    public:
        explicit PostProcessStack(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override;

        /// The fields as the renderer takes them (not yet sanitized)
        [[nodiscard]] Renderer::Common::PostProcessSettings GetSettings() const;
        /// Sets every field from `settings`
        void SetSettings(const Renderer::Common::PostProcessSettings &settings);

        bool bloom = false;
        float bloomThreshold = 0.7f;
        float bloomIntensity = 1.0f;
        int bloomIterations = 4;
        /// rgb multiply the glow; alpha is ignored
        Common::Color bloomTint = Common::Color::White;
        float invert = 0.0f;
        float chromaticAberration = 0.0f;
        float vignette = 0.0f;
    };
}
