// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Pédales intégrées de JSG Guitar — copie de Source/Pedals.h, sans JUCE.
    Le traitement du son est identique ; seules les fonctions de JUCE sont remplacées
    par leurs équivalents de dsp.h.
*/

#include "dsp.h"

#include <array>
#include <memory>

namespace jsg
{

inline constexpr int numSlots = 16;        // 2 rangées de 8 emplacements
inline constexpr int slotsPerRow = 8;      // rangée 1 : avant l'ampli ; rangée 2 : après le baffle
inline constexpr int maxPedalKnobs = 4;

/** Conversion position (0..1) -> valeur réelle d'un potard. */
struct KnobRange
{
    float min = 0.0f, max = 1.0f;
    bool  logarithmic = false;
    int   steps = 0;

    float toReal (float position) const noexcept
    {
        const float p = jlimit (0.0f, 1.0f, position);
        if (steps > 1)   return std::round (p * (float) (steps - 1));
        return logarithmic ? min * std::pow (max / min, p) : min + (max - min) * p;
    }
};

class Pedal
{
public:
    virtual ~Pedal() = default;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;
    virtual void process (float* left, float* right, int numSamples, const float* knobs) noexcept = 0;
    virtual bool resetsWhenSwitchedOff() const noexcept { return true; }

protected:
    static float timeCoef (double sampleRate, double seconds) noexcept
    {
        return 1.0f - std::exp (-1.0f / (float) (sampleRate * jmax (1.0e-5, seconds)));
    }
};

//==============================================================================
class NoiseGatePedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override
    {
        fs = sampleRate;
        envRelease = std::exp (-1.0f / (float) (fs * 0.030));
        attack = timeCoef (fs, 0.0015);
        reset();
    }

    void reset() override { env = 0.0f; gain = 1.0f; open = true; }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const float threshold = dbToGain (knobs[0]);
        const float release = timeCoef (fs, 0.001 * (double) knobs[1]);

        for (int i = 0; i < n; ++i)
        {
            const float a = jmax (std::abs (left[i]), std::abs (right[i]));
            env = a > env ? a : env * envRelease;

            if (env > threshold)             open = true;
            else if (env < threshold * 0.5f) open = false;

            const float target = open ? 1.0f : 0.0f;
            gain += (target - gain) * (target > gain ? attack : release);
            gain = undenormalise (gain);
            left[i] *= gain;
            right[i] *= gain;
        }
    }

private:
    double fs = 48000.0;
    float envRelease = 0.999f, attack = 0.1f;
    float env = 0.0f, gain = 1.0f;
    bool open = true;
};

//==============================================================================
class CompressorPedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override
    {
        fs = sampleRate;
        release = timeCoef (fs, 0.150);
        reset();
    }

    void reset() override { env = 0.0f; }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const float attack    = timeCoef (fs, 0.001 * (double) knobs[1]);
        const float threshold = -18.0f - 2.4f * knobs[0];
        const float slope     = 0.75f;
        const float knee      = 6.0f;
        const float makeup    = -threshold * slope * 0.65f + (knobs[2] - 5.0f) * 2.4f;

        for (int i = 0; i < n; ++i)
        {
            const float a = jmax (std::abs (left[i]), std::abs (right[i]));
            env += (a - env) * (a > env ? attack : release);

            const float over = 20.0f * std::log10 (env + 1.0e-9f) - threshold;
            float reduction = 0.0f;

            if (over >= knee * 0.5f)        reduction = over * slope;
            else if (over > -knee * 0.5f)   reduction = (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee) * slope;

            const float g = std::pow (10.0f, (makeup - reduction) * 0.05f);
            left[i] *= g;
            right[i] *= g;
        }
    }

private:
    double fs = 48000.0;
    float release = 0.001f, env = 0.0f;
};

//==============================================================================
class DrivePedal final : public Pedal
{
public:
    enum class Voice { screamer, overdrive, distortion, fuzz };

    explicit DrivePedal (Voice voiceToUse) : voice (voiceToUse) {}

    void prepare (double sampleRate, int maxBlockSize) override
    {
        osRate = sampleRate * 4.0;
        oversampler.prepare (maxBlockSize);
        mono.assign ((size_t) maxBlockSize, 0.0f);
        smooth = (double) timeCoef (osRate, 0.020);
        fuzzCoef = (double) timeCoef (osRate, 0.015);
        highPass.setHighPass (osRate, config().highPassHz, 0.7071);
        shownTone = -1.0f;
        reset();
    }

    void reset() override
    {
        oversampler.reset();
        highPass.reset();
        lowPass.reset();
        gain = -1.0;
        fuzzEnv = 0.0;
    }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const auto c = config();
        const double drive = (double) jlimit (0.0f, 10.0f, knobs[0]) * 0.1;
        const double target = std::pow (10.0, jmap (drive, c.minDb, c.maxDb) / 20.0);
        const double level = 0.35 * std::pow (10.0, ((double) knobs[2] - 5.0) * 2.4 / 20.0);
        if (gain < 0.0) gain = target;

        if (std::abs (knobs[1] - shownTone) > 1.0e-3f)
        {
            shownTone = knobs[1];
            lowPass.setLowPass (osRate, c.toneMinHz * std::pow (c.toneMaxHz / c.toneMinHz, (double) shownTone * 0.1), 0.7071);
        }

        for (int i = 0; i < n; ++i)
            mono[(size_t) i] = 0.5f * (left[i] + right[i]);

        float* u = oversampler.up (mono.data(), n);
        const int upN = n * 4;

        for (int j = 0; j < upN; ++j)
        {
            gain += (target - gain) * smooth;

            const double v = (double) u[j];
            const double x = highPass.process (v) * gain;
            double y;

            switch (voice)
            {
                case Voice::screamer:   y = std::tanh (x) + v;                                   break;
                case Voice::distortion: y = x / std::sqrt (std::sqrt (1.0 + x * x * x * x));     break;
                case Voice::fuzz:
                {
                    fuzzEnv += (std::abs (x) - fuzzEnv) * fuzzCoef;
                    const double shifted = x - 0.45 * fuzzEnv;
                    y = shifted >= 0.0 ? 1.0 - std::exp (-shifted) : -0.6 * (1.0 - std::exp (2.5 * shifted));
                    break;
                }
                case Voice::overdrive:
                default:                y = std::tanh (x + 0.3) - std::tanh (0.3);               break;
            }

            u[j] = (float) (lowPass.process (y) * level);
        }

        oversampler.down (mono.data(), n);

        std::copy_n (mono.data(), n, left);
        std::copy_n (mono.data(), n, right);
    }

private:
    struct Config { double highPassHz, minDb, maxDb, toneMinHz, toneMaxHz; };

    Config config() const noexcept
    {
        switch (voice)
        {
            case Voice::screamer:   return { 720.0,  6.0, 40.0, 1500.0, 6500.0 };
            case Voice::distortion: return {  90.0, 10.0, 50.0, 1800.0, 9000.0 };
            case Voice::fuzz:       return {  50.0, 20.0, 56.0, 1200.0, 6000.0 };
            case Voice::overdrive:
            default:                return { 120.0,  0.0, 36.0, 1800.0, 9000.0 };
        }
    }

    Voice voice;
    double osRate = 192000.0;
    Oversampler4 oversampler;
    std::vector<float> mono;
    Biquad highPass, lowPass;
    double gain = -1.0, smooth = 0.001, fuzzEnv = 0.0, fuzzCoef = 0.001;
    float shownTone = -1.0f;
};

//==============================================================================
class DelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        buffer.assign ((size_t) nextPowerOfTwo (maxDelaySamples + 8), 0.0f);
        mask = (int) buffer.size() - 1;
        writeIndex = 0;
    }

    void reset() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); writeIndex = 0; }

    inline void write (float value) noexcept
    {
        buffer[(size_t) writeIndex] = value;
        writeIndex = (writeIndex + 1) & mask;
    }

    inline float read (float delay) const noexcept
    {
        const float position = (float) writeIndex - delay;
        const int   whole    = (int) std::floor (position);
        const float frac     = position - (float) whole;
        const float a = buffer[(size_t) (whole & mask)];
        const float b = buffer[(size_t) ((whole + 1) & mask)];
        return a + (b - a) * frac;
    }

private:
    std::vector<float> buffer;
    int mask = 0, writeIndex = 0;
};

//==============================================================================
class ChorusPedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override
    {
        fs = sampleRate;
        for (auto& line : lines) line.prepare ((int) (fs * 0.05));
        reset();
    }

    void reset() override
    {
        for (auto& line : lines) line.reset();
        phase = 0.0;
    }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const double step = twoPi * (double) knobs[0] / fs;
        const float base  = (float) (fs * 0.012);
        const float depth = (float) (fs * 0.0055) * knobs[1] * 0.1f;
        const float mix   = knobs[2] * 0.1f;
        const float dry   = 1.0f - 0.35f * mix;

        for (int i = 0; i < n; ++i)
        {
            lines[0].write (left[i]);
            lines[1].write (right[i]);

            const float wetL = lines[0].read (1.0f + base + depth * (float) std::sin (phase));
            const float wetR = lines[1].read (1.0f + base + depth * (float) std::cos (phase));

            left[i]  = left[i]  * dry + wetL * mix * 0.7f;
            right[i] = right[i] * dry + wetR * mix * 0.7f;

            phase += step;
            if (phase > twoPi) phase -= twoPi;
        }
    }

private:
    double fs = 48000.0, phase = 0.0;
    std::array<DelayLine, 2> lines;
};

//==============================================================================
class DelayPedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override
    {
        fs = sampleRate;
        for (auto& line : lines) line.prepare ((int) (fs * 1.3));
        toneCoef = timeCoef (fs, 1.0 / (twoPi * 3800.0));
        glide = timeCoef (fs, 0.24);
        reset();
    }

    void reset() override
    {
        for (auto& line : lines) line.reset();
        tone = { 0.0f, 0.0f };
        delaySamples = -1.0f;
    }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const float target   = (float) (fs * 0.001) * knobs[0];
        const float feedback = jlimit (0.0f, 0.9f, knobs[1] * 0.09f);
        const float level    = knobs[2] * 0.1f;
        if (delaySamples < 0.0f) delaySamples = target;

        float* io[2] = { left, right };

        for (int i = 0; i < n; ++i)
        {
            delaySamples += (target - delaySamples) * glide;

            for (size_t ch = 0; ch < 2; ++ch)
            {
                const float echo = lines[ch].read (delaySamples);
                tone[ch] = undenormalise (tone[ch] + (echo - tone[ch]) * toneCoef);
                lines[ch].write (io[ch][i] + tone[ch] * feedback);
                io[ch][i] += tone[ch] * level;
            }
        }
    }

private:
    double fs = 48000.0;
    std::array<DelayLine, 2> lines;
    std::array<float, 2> tone { 0.0f, 0.0f };
    float toneCoef = 0.4f, glide = 0.0001f, delaySamples = -1.0f;
};

//==============================================================================
class ReverbPedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override   { reverb.setSampleRate (sampleRate); reverb.reset(); }
    void reset() override                            { reverb.reset(); }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const float mix = knobs[0] * 0.1f;

        Reverb::Parameters p;
        p.roomSize   = 0.30f + 0.065f * knobs[1];
        p.damping    = 0.85f - 0.07f * knobs[2];
        p.width      = 1.0f;
        p.wetLevel   = 0.25f * mix;
        p.dryLevel   = 0.5f * (1.0f - 0.25f * mix);
        p.freezeMode = 0.0f;
        reverb.setParameters (p);
        reverb.processStereo (left, right, n);
    }

private:
    Reverb reverb;
};

//==============================================================================
class TremoloPedal final : public Pedal
{
public:
    void prepare (double sampleRate, int) override { fs = sampleRate; reset(); }
    void reset() override { phase = 0.0; }

    void process (float* left, float* right, int n, const float* knobs) noexcept override
    {
        const double step  = twoPi * (double) knobs[0] / fs;
        const float  depth = knobs[1] * 0.1f;

        for (int i = 0; i < n; ++i)
        {
            const float gain = 1.0f - depth * (0.5f - 0.5f * (float) std::cos (phase));
            left[i]  *= gain;
            right[i] *= gain;

            phase += step;
            if (phase > twoPi) phase -= twoPi;
        }
    }

private:
    double fs = 48000.0, phase = 0.0;
};

//==============================================================================
/** Le catalogue, dans le même ordre que Source/Pedals.h (et que pedals.js côté interface). */
struct PedalType
{
    std::unique_ptr<Pedal> (*create)();
    std::array<KnobRange, maxPedalKnobs> ranges;
};

namespace detail
{
    template <typename T> std::unique_ptr<Pedal> make() { return std::make_unique<T>(); }
    template <DrivePedal::Voice v> std::unique_ptr<Pedal> makeDrive() { return std::make_unique<DrivePedal> (v); }
    inline constexpr KnobRange k10 { 0.0f, 10.0f, false, 0 };
    inline constexpr KnobRange none { 0.0f, 1.0f, false, 0 };
}

inline const std::array<PedalType, 10>& pedalTypes()
{
    using namespace detail;
    using V = DrivePedal::Voice;

    static const std::array<PedalType, 10> list { {
        { &make<NoiseGatePedal>,  { { { -80.0f, -20.0f, false, 0 }, { 10.0f, 1000.0f, true, 0 }, none, none } } },   // gate
        { &make<CompressorPedal>, { { k10, { 1.0f, 100.0f, true, 0 }, k10, none } } },                               // compressor
        { &makeDrive<V::screamer>,   { { k10, k10, k10, none } } },                                                 // screamer
        { &makeDrive<V::overdrive>,  { { k10, k10, k10, none } } },                                                 // overdrive
        { &makeDrive<V::distortion>, { { k10, k10, k10, none } } },                                                 // distortion
        { &makeDrive<V::fuzz>,       { { k10, k10, k10, none } } },                                                 // fuzz
        { &make<ChorusPedal>,     { { { 0.1f, 5.0f, true, 0 }, k10, k10, none } } },                                // chorus
        { &make<TremoloPedal>,    { { { 1.0f, 12.0f, true, 0 }, k10, none, none } } },                              // tremolo
        { &make<DelayPedal>,      { { { 40.0f, 1200.0f, true, 0 }, k10, k10, none } } },                            // delay
        { &make<ReverbPedal>,     { { k10, k10, k10, none } } },                                                    // reverb
    } };
    return list;
}

} // namespace jsg
