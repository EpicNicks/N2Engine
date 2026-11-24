#include "engine/rendering/PostProcessStack.hpp"

#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    using Renderer::Common::PostProcessSettings;

    PostProcessStack::PostProcessStack(GameObject &gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(bloom), bloom).Tooltip("Glow around the parts brighter than the threshold");
        RegisterMember(NAMEOF(bloomThreshold), bloomThreshold).Range(0.0, 1.0)
            .Tooltip("Brightness (the largest of r, g, b) above which a pixel glows");
        RegisterMember(NAMEOF(bloomIntensity), bloomIntensity).Range(0.0, PostProcessSettings::MaxBloomIntensity)
            .Tooltip("How much of the glow is added back");
        RegisterMember(NAMEOF(bloomIterations), bloomIterations).Range(1.0, PostProcessSettings::MaxBloomIterations)
            .Tooltip("Blur passes: the glow widens with each");
        RegisterMember(NAMEOF(bloomTint), bloomTint).Tooltip("The glow's colour");
        RegisterMember(NAMEOF(invert), invert).Range(0.0, 1.0).Tooltip("0 none, 1 a full negative");
        RegisterMember(NAMEOF(chromaticAberration), chromaticAberration)
            .Range(0.0, PostProcessSettings::MaxChromaticAberration)
            .Tooltip("Red and blue fringes, growing towards the edges (0.02 is strong)");
        RegisterMember(NAMEOF(vignette), vignette).Range(0.0, 1.0).Tooltip("How dark the corners get");
    }

    std::string PostProcessStack::GetTypeName() const
    {
        return NAMEOF(PostProcessStack);
    }

    PostProcessSettings PostProcessStack::GetSettings() const
    {
        PostProcessSettings settings;
        settings.bloom = bloom;
        settings.bloomThreshold = bloomThreshold;
        settings.bloomIntensity = bloomIntensity;
        settings.bloomIterations = bloomIterations;
        settings.bloomTint[0] = bloomTint.r;
        settings.bloomTint[1] = bloomTint.g;
        settings.bloomTint[2] = bloomTint.b;
        settings.invert = invert;
        settings.chromaticAberration = chromaticAberration;
        settings.vignette = vignette;
        return settings;
    }

    void PostProcessStack::SetSettings(const PostProcessSettings &settings)
    {
        bloom = settings.bloom;
        bloomThreshold = settings.bloomThreshold;
        bloomIntensity = settings.bloomIntensity;
        bloomIterations = settings.bloomIterations;
        bloomTint.r = settings.bloomTint[0];
        bloomTint.g = settings.bloomTint[1];
        bloomTint.b = settings.bloomTint[2];
        invert = settings.invert;
        chromaticAberration = settings.chromaticAberration;
        vignette = settings.vignette;
    }
}
