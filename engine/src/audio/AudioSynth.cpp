#include "engine/audio/AudioSynth.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace N2Engine::Audio
{
    namespace
    {
        void PutSample(AudioData &out, const std::size_t index, const double value)
        {
            const double clamped = std::clamp(value, -1.0, 1.0);
            const auto sample = static_cast<std::int16_t>(std::lround(clamped * 32767.0));
            std::memcpy(out.samples.data() + index * sizeof(std::int16_t), &sample, sizeof(sample));
        }

        /// One oscillator value at phase p (0 <= p < 1)
        double Oscillator(const Waveform waveform, const double p, std::uint32_t &noiseState)
        {
            switch (waveform)
            {
            case Waveform::Sine:
                return std::sin(2.0 * std::numbers::pi * p);
            case Waveform::Square:
                return p < 0.5 ? 1.0 : -1.0;
            case Waveform::Saw:
                return p < 0.5 ? 2.0 * p : 2.0 * p - 2.0;
            case Waveform::Triangle:
                if (p < 0.25)
                {
                    return 4.0 * p;
                }
                return p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0;
            case Waveform::Noise:
                // xorshift32: small, fast and the same on every platform
                noiseState ^= noiseState << 13;
                noiseState ^= noiseState >> 17;
                noiseState ^= noiseState << 5;
                return static_cast<double>(noiseState) / 4294967295.0 * 2.0 - 1.0;
            }
            return 0.0;
        }

        bool IsFiniteNonNegative(const double value) { return std::isfinite(value) && value >= 0.0; }
    }

    std::optional<Waveform> WaveformFromString(const std::string_view name)
    {
        if (name == "sine") return Waveform::Sine;
        if (name == "square") return Waveform::Square;
        if (name == "saw") return Waveform::Saw;
        if (name == "triangle") return Waveform::Triangle;
        if (name == "noise") return Waveform::Noise;
        return std::nullopt;
    }

    std::optional<std::string> GenerateTone(const ToneParams &params, AudioData &out)
    {
        if (params.sampleRate < MinSynthSampleRate || params.sampleRate > MaxSynthSampleRate)
        {
            return std::format("sampleRate must be {}..{} (got {})", MinSynthSampleRate, MaxSynthSampleRate,
                               params.sampleRate);
        }
        const double rate = params.sampleRate;
        if (!std::isfinite(params.frequency) || params.frequency <= 0.0 || params.frequency > rate / 2.0)
        {
            return std::format("frequency must be more than 0 and at most half the sample rate ({} Hz)", rate / 2.0);
        }
        if (!std::isfinite(params.duration) || params.duration <= 0.0 || params.duration > MaxSynthSeconds)
        {
            return std::format("duration must be more than 0 and at most {} seconds", MaxSynthSeconds);
        }
        if (!std::isfinite(params.volume) || params.volume < 0.0 || params.volume > 1.0)
        {
            return "volume must be 0..1";
        }
        if (!std::isfinite(params.sustain) || params.sustain < 0.0 || params.sustain > 1.0)
        {
            return "sustain must be 0..1";
        }
        if (!IsFiniteNonNegative(params.attack) || !IsFiniteNonNegative(params.decay) ||
            !IsFiniteNonNegative(params.release))
        {
            return "attack, decay and release must be 0 or more seconds";
        }

        const auto frames = static_cast<std::size_t>(std::llround(params.duration * rate));
        if (frames == 0)
        {
            return "duration is shorter than one sample";
        }

        // Envelope stage lengths in frames, scaled down together when they don't fit in the clip
        double attack = params.attack * rate;
        double decay = params.decay * rate;
        double release = params.release * rate;
        const double total = attack + decay + release;
        const auto length = static_cast<double>(frames);
        if (total > length)
        {
            const double scale = length / total;
            attack *= scale;
            decay *= scale;
            release *= scale;
        }
        const double releaseStart = release >= 1.0 ? length - release : length;

        out.sampleRate = params.sampleRate;
        out.channels = 1;
        out.bitsPerSample = 16;
        out.samples.assign(frames * sizeof(std::int16_t), 0);

        std::uint32_t noiseState = params.seed != 0 ? params.seed : 1u;
        for (std::size_t i = 0; i < frames; ++i)
        {
            const auto t = static_cast<double>(i);
            double envelope = params.sustain;
            if (t >= releaseStart)
            {
                // Down to exactly 0 on the last frame
                const double span = std::max(length - 1.0 - releaseStart, 1.0);
                envelope = params.sustain * std::clamp((length - 1.0 - t) / span, 0.0, 1.0);
            }
            else if (t < attack)
            {
                envelope = t / attack;
            }
            else if (t < attack + decay)
            {
                envelope = 1.0 - (1.0 - params.sustain) * (t - attack) / decay;
            }

            // frequency * t first: exact for whole-number frequencies and times, so a cycle's halves land on whole samples
            const double cycles = params.frequency * t / rate;
            const double phase = cycles - std::floor(cycles);
            PutSample(out, i, params.volume * envelope * Oscillator(params.waveform, phase, noiseState));
        }
        return std::nullopt;
    }

    std::optional<std::string> PcmFromFloats(const std::span<const float> samples, const int sampleRate,
                                             const int channels, AudioData &out)
    {
        if (channels != 1 && channels != 2)
        {
            return std::format("channels must be 1 or 2 (got {})", channels);
        }
        if (sampleRate < MinSynthSampleRate || sampleRate > MaxSynthSampleRate)
        {
            return std::format("sampleRate must be {}..{} (got {})", MinSynthSampleRate, MaxSynthSampleRate, sampleRate);
        }
        if (samples.empty())
        {
            return "there are no samples";
        }
        if (samples.size() % static_cast<std::size_t>(channels) != 0)
        {
            return std::format("{} samples is not a whole number of {}-channel frames", samples.size(), channels);
        }
        const std::size_t frames = samples.size() / static_cast<std::size_t>(channels);
        const auto maxFrames = static_cast<std::size_t>(MaxSynthSeconds * sampleRate);
        if (frames > maxFrames)
        {
            return std::format("{} frames is longer than {} seconds at {} Hz", frames, MaxSynthSeconds, sampleRate);
        }
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            if (!std::isfinite(samples[i]))
            {
                return std::format("sample {} is not a finite number", i + 1);
            }
        }

        out.sampleRate = sampleRate;
        out.channels = channels;
        out.bitsPerSample = 16;
        out.samples.assign(samples.size() * sizeof(std::int16_t), 0);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            PutSample(out, i, static_cast<double>(samples[i]));
        }
        return std::nullopt;
    }
}
