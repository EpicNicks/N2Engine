#include "engine/rendering/PostProcessStack.hpp"

#include <cmath>

#include "engine/Time.hpp"
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
        RegisterMember(NAMEOF(blur), blur).Tooltip("Blur the whole scene, before the other effects");
        RegisterMember(NAMEOF(blurStrength), blurStrength).Range(0.0, 1.0)
            .Tooltip("0 the sharp image, 1 the blurred one");
        RegisterMember(NAMEOF(blurIterations), blurIterations).Range(1.0, PostProcessSettings::MaxBlurIterations)
            .Tooltip("Blur passes: the blur widens with each");
        RegisterMember(NAMEOF(distortion), distortion).Range(-1.0, 1.0)
            .Tooltip("Lens distortion: above 0 the middle bulges, below 0 it pinches");
        RegisterMember(NAMEOF(waveAmplitude), waveAmplitude).Range(0.0, PostProcessSettings::MaxWaveAmplitude)
            .Tooltip("How far the wave moves a point, as a fraction of the screen");
        RegisterMember(NAMEOF(waveFrequency), waveFrequency).Range(0.0, PostProcessSettings::MaxWaveFrequency)
            .Tooltip("Waves from one edge to the other");
        RegisterMember(NAMEOF(wavePhase), wavePhase).Tooltip("Where the wave is in its cycle, in cycles");
        RegisterMember(NAMEOF(waveSpeed), waveSpeed).Tooltip("Cycles a second the wave travels by itself");
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
        settings.blur = blur;
        settings.blurStrength = blurStrength;
        settings.blurIterations = blurIterations;
        settings.distortion = distortion;
        settings.waveAmplitude = waveAmplitude;
        settings.waveFrequency = waveFrequency;
        settings.wavePhase = wavePhase;
        return settings;
    }

    PostProcessSettings PostProcessStack::GetFrameSettings() const
    {
        PostProcessSettings settings = GetSettings();
        // In double, so a long run keeps its fraction of a cycle
        const double travelled = std::isfinite(waveSpeed)
                                     ? static_cast<double>(waveSpeed) * static_cast<double>(Time::GetUnscaledTime())
                                     : 0.0;
        settings.wavePhase = wavePhase + static_cast<float>(travelled - std::floor(travelled));
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
        blur = settings.blur;
        blurStrength = settings.blurStrength;
        blurIterations = settings.blurIterations;
        distortion = settings.distortion;
        waveAmplitude = settings.waveAmplitude;
        waveFrequency = settings.waveFrequency;
        wavePhase = settings.wavePhase;
    }
}
