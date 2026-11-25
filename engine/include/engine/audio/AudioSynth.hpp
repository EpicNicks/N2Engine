#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "AudioClip.hpp"

namespace N2Engine::Audio
{
    /// The tone generator's oscillators. Every waveform starts at phase 0: sine, saw and triangle at 0 rising,
    /// square at +1. Noise is white (uniform), from a fixed-seed generator, so the same parameters make the same clip.
    enum class Waveform
    {
        Sine,
        Square,
        Saw,
        Triangle,
        Noise,
    };

    /// "sine", "square", "saw", "triangle", "noise" (as Lua names them); nullopt for anything else
    [[nodiscard]] std::optional<Waveform> WaveformFromString(std::string_view name);

    /// One synthesised note. The clip is exactly round(duration * sampleRate) frames of mono 16-bit audio.
    /// The envelope is a linear ADSR inside that length: attack from 0 up to 1, decay down to sustain, hold at
    /// sustain, and release down to 0 over the clip's last `release` seconds. When attack + decay + release is longer
    /// than the clip, the three are scaled down together to fit.
    struct ToneParams
    {
        Waveform waveform = Waveform::Sine;
        double frequency = 440.0;  // Hz, more than 0 and at most sampleRate / 2
        double duration = 0.25;    // seconds, more than 0 and at most MaxSynthSeconds
        double volume = 0.5;       // peak amplitude, 0..1
        double attack = 0.005;     // seconds, 0 or more
        double decay = 0.05;       // seconds, 0 or more
        double sustain = 0.7;      // level, 0..1
        double release = 0.05;     // seconds, 0 or more
        int sampleRate = 48000;    // MinSynthSampleRate..MaxSynthSampleRate
        std::uint32_t seed = 1;    // the noise generator's seed (0 is taken as 1)
    };

    /// Longest clip the generators make (tone or PCM)
    inline constexpr double MaxSynthSeconds = 30.0;
    inline constexpr int MinSynthSampleRate = 8000;
    inline constexpr int MaxSynthSampleRate = 192000;

    /// Fills `out` with the tone's samples (mono, 16-bit), or returns why the parameters are invalid
    /// (nothing is written then). Needs no audio device: AudioClip::CreateFromData makes the clip from the data.
    [[nodiscard]] std::optional<std::string> GenerateTone(const ToneParams &params, AudioData &out);

    /// Converts float PCM (nominally -1..1, clamped; interleaved when channels is 2) to 16-bit AudioData, or returns
    /// why it is invalid: no samples, 1 or 2 channels only, a count that is not a whole number of frames, a rate
    /// outside MinSynthSampleRate..MaxSynthSampleRate, more than MaxSynthSeconds, or a NaN or infinite sample.
    [[nodiscard]] std::optional<std::string> PcmFromFloats(std::span<const float> samples, int sampleRate, int channels,
                                                           AudioData &out);
}
