// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    JSG Guitar — version navigateur : outils de traitement du son du moteur audio.
      - petites fonctions (jlimit, jmap, décibels…) ;
      - Biquad : filtres du plug-in (formules publiques de R. Bristow-Johnson) ;
      - Oversampler4 : suréchantillonnage x4 par filtres demi-bande IIR polyphasés,
        coefficients calculés par la méthode de L. de Soras (hiir, licence WTFPL) ;
      - Reverb : algorithme Freeverb de Jezar at Dreampoint (domaine public) ;
      - Convolution : convolution partitionnée par FFT, sans latence, pour les réponses
        impulsionnelles (IR rognée et normalisée).
    Tout ce fichier est écrit pour JSG Guitar ; il n'utilise ni ne contient de code de JUCE.
*/

#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <array>

namespace jsg
{

//==============================================================================
template <typename T> inline T jlimit (T lo, T hi, T v) noexcept   { return v < lo ? lo : (hi < v ? hi : v); }
template <typename T> inline T jmax (T a, T b) noexcept            { return a < b ? b : a; }
template <typename T> inline T jmax (T a, T b, T c) noexcept       { return jmax (jmax (a, b), c); }
template <typename T> inline T jmin (T a, T b) noexcept            { return b < a ? b : a; }
template <typename T> inline T jmap (T v, T outMin, T outMax) noexcept { return outMin + v * (outMax - outMin); }
template <typename T> inline T jmap (T v, T inMin, T inMax, T outMin, T outMax) noexcept
{ return outMin + (outMax - outMin) * (v - inMin) / (inMax - inMin); }

inline float dbToGain (float db) noexcept   { return db > -100.0f ? std::pow (10.0f, db * 0.05f) : 0.0f; }
inline int nextPowerOfTwo (int n) noexcept  { int p = 1; while (p < n) p <<= 1; return p; }

constexpr double pi = 3.14159265358979323846;
constexpr double twoPi = 2.0 * pi;

/** Évite les nombres « dénormalisés » (très lents) dans les boucles de réinjection. */
inline float undenormalise (float x) noexcept { return (x > -1.0e-15f && x < 1.0e-15f) ? 0.0f : x; }

//==============================================================================
/** Filtre biquad (formules RBJ), forme transposée II, calcul en double précision. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setBypass() noexcept { b0 = 1.0; b1 = b2 = a1 = a2 = 0.0; }

    void setLowPass (double fs, double f, double q) noexcept
    {
        const auto k = prewarp (fs, f, q);
        set ((1.0 - k.c) * 0.5, 1.0 - k.c, (1.0 - k.c) * 0.5, 1.0 + k.alpha, -2.0 * k.c, 1.0 - k.alpha);
    }

    void setHighPass (double fs, double f, double q) noexcept
    {
        const auto k = prewarp (fs, f, q);
        set ((1.0 + k.c) * 0.5, -(1.0 + k.c), (1.0 + k.c) * 0.5, 1.0 + k.alpha, -2.0 * k.c, 1.0 - k.alpha);
    }

    void setBandPass (double fs, double f, double q) noexcept
    {
        const auto k = prewarp (fs, f, q);
        set (k.alpha, 0.0, -k.alpha, 1.0 + k.alpha, -2.0 * k.c, 1.0 - k.alpha);
    }

    void setPeak (double fs, double f, double q, double gainDb) noexcept
    {
        const auto k = prewarp (fs, f, q);
        const double A = std::pow (10.0, gainDb / 40.0);
        set (1.0 + k.alpha * A, -2.0 * k.c, 1.0 - k.alpha * A,
             1.0 + k.alpha / A, -2.0 * k.c, 1.0 - k.alpha / A);
    }

    void setLowShelf (double fs, double f, double gainDb) noexcept
    {
        const auto k = prewarp (fs, f, 0.7071);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double s = 2.0 * std::sqrt (A) * k.alpha;
        set (A * ((A + 1.0) - (A - 1.0) * k.c + s),
             2.0 * A * ((A - 1.0) - (A + 1.0) * k.c),
             A * ((A + 1.0) - (A - 1.0) * k.c - s),
             (A + 1.0) + (A - 1.0) * k.c + s,
             -2.0 * ((A - 1.0) + (A + 1.0) * k.c),
             (A + 1.0) + (A - 1.0) * k.c - s);
    }

    void setHighShelf (double fs, double f, double gainDb) noexcept
    {
        const auto k = prewarp (fs, f, 0.7071);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double s = 2.0 * std::sqrt (A) * k.alpha;
        set (A * ((A + 1.0) + (A - 1.0) * k.c + s),
             -2.0 * A * ((A - 1.0) + (A + 1.0) * k.c),
             A * ((A + 1.0) + (A - 1.0) * k.c - s),
             (A + 1.0) - (A - 1.0) * k.c + s,
             2.0 * ((A - 1.0) - (A + 1.0) * k.c),
             (A + 1.0) - (A - 1.0) * k.c - s);
    }

private:
    struct Warp { double c, alpha; };

    static Warp prewarp (double fs, double f, double q) noexcept
    {
        const double fc = jlimit (5.0, fs * 0.45, f);
        const double w0 = twoPi * fc / fs;
        return { std::cos (w0), std::sin (w0) / (2.0 * q) };
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        const double inv = 1.0 / na0;
        b0 = nb0 * inv;  b1 = nb1 * inv;  b2 = nb2 * inv;
        a1 = na1 * inv;  a2 = na2 * inv;
    }
};

//==============================================================================
//  SURÉCHANTILLONNAGE
//  Filtres demi-bande IIR polyphasés (deux chemins de passe-tout du premier ordre).
//  Calcul des coefficients : méthode elliptique de Laurent de Soras, bibliothèque « hiir »
//  (licence WTFPL : libre de toute contrainte). Code écrit pour JSG Guitar.
//==============================================================================
namespace halfband
{
    /** Paramètres de transition (k, q) pour une bande de transition normalisée (0..0,5). */
    inline void transitionParams (double transition, double& k, double& q)
    {
        k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
        k *= k;
        const double kk = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
        const double e2 = e * e, e4 = e2 * e2;
        q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    }

    /** Ordre du filtre nécessaire pour atteindre l'atténuation voulue (en dB, positive). */
    inline int orderFor (double attenuationDb, double q)
    {
        const double a2 = std::pow (10.0, -attenuationDb / 10.0);
        const double a = a2 / (1.0 - a2);
        int order = (int) std::ceil (std::log (a * a / 16.0) / std::log (q));
        if ((order & 1) == 0) ++order;
        if (order < 3) order = 3;
        return order;
    }

    inline double numeratorSeries (double q, int order, int c)
    {
        double sum = 0.0, term;
        int i = 0, sign = 1;
        do
        {
            term = std::pow (q, (double) (i * (i + 1))) * std::sin ((i * 2 + 1) * c * pi / order) * sign;
            sum += term;
            sign = -sign;
            ++i;
        } while (std::abs (term) > 1e-100);
        return sum;
    }

    inline double denominatorSeries (double q, int order, int c)
    {
        double sum = 0.0, term;
        int i = 1, sign = -1;
        do
        {
            term = std::pow (q, (double) (i * i)) * std::cos (i * 2 * c * pi / order) * sign;
            sum += term;
            sign = -sign;
            ++i;
        } while (std::abs (term) > 1e-100);
        return sum;
    }

    /** Coefficients des passe-tout, du plus petit au plus grand. */
    inline std::vector<double> design (double transition, double attenuationDb)
    {
        double k, q;
        transitionParams (transition, k, q);
        const int order = orderFor (attenuationDb, q);
        std::vector<double> coefs;

        for (int index = 0; index < (order - 1) / 2; ++index)
        {
            const int c = index + 1;
            const double num = numeratorSeries (q, order, c) * std::pow (q, 0.25);
            const double den = denominatorSeries (q, order, c) + 0.5;
            const double ww = num / den, wwsq = ww * ww;
            const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
            coefs.push_back ((1.0 - x) / (1.0 + x));
        }
        return coefs;
    }

    /** Chaîne de passe-tout du premier ordre en z² : y = a·x + s ; s = x − a·y. */
    struct AllpassChain
    {
        std::vector<double> a, s;

        void setup (const std::vector<double>& coefs) { a = coefs; s.assign (a.size(), 0.0); }
        void clear() noexcept { std::fill (s.begin(), s.end(), 0.0); }

        inline double run (double x) noexcept
        {
            for (size_t i = 0; i < a.size(); ++i)
            {
                const double y = a[i] * x + s[i];
                s[i] = x - a[i] * y;
                x = y;
            }
            return x;
        }
    };

    /** Répartit les coefficients : rangs pairs sur le chemin A, impairs sur le chemin B.
        Le filtre demi-bande vaut H(z) = ½ (A(z²) + z⁻¹ B(z²)). */
    inline void split (const std::vector<double>& coefs, AllpassChain& pathA, AllpassChain& pathB)
    {
        std::vector<double> even, odd;
        for (size_t i = 0; i < coefs.size(); ++i) (i % 2 == 0 ? even : odd).push_back (coefs[i]);
        pathA.setup (even);
        pathB.setup (odd);
    }
}

/** Un étage x2, montée et descente (chacune avec son propre filtre). */
class HalfBandStage
{
public:
    /** transition : largeur de la bande de transition (0..0,5) ; attenuation en dB (positive). */
    void design (double transitionUp, double attenuationUp, double transitionDown, double attenuationDown)
    {
        halfband::split (halfband::design (transitionUp, attenuationUp), upA, upB);
        halfband::split (halfband::design (transitionDown, attenuationDown), downA, downB);
        reset();
    }

    void reset() noexcept
    {
        upA.clear(); upB.clear(); downA.clear(); downB.clear();
        pendingB = 0.0;
    }

    /** n échantillons -> 2n : chaque chemin donne un des deux échantillons de sortie. */
    void upsample (const float* in, float* out, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const double x = in[i];
            out[2 * i]     = (float) upA.run (x);
            out[2 * i + 1] = (float) upB.run (x);
        }
    }

    /** 2n échantillons -> n : pairs dans A, impairs dans B (retardé d'un pas), moyenne des deux. */
    void downsample (const float* in, float* out, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const double a = downA.run (in[2 * i]);
            out[i] = (float) (0.5 * (a + pendingB));
            pendingB = downB.run (in[2 * i + 1]);
        }
    }

private:
    halfband::AllpassChain upA, upB, downA, downB;
    double pendingB = 0.0;
};

/** Suréchantillonnage x4 en deux étages x2. Le premier étage (le plus près de la fréquence
    d'origine) a la transition la plus étroite ; le second, qui travaille plus haut, peut être plus doux. */
class Oversampler4
{
public:
    void prepare (int maxBlock)
    {
        stages[0].design (0.05, 75.0, 0.06, 70.0);
        stages[1].design (0.10, 65.0, 0.12, 60.0);
        mid.assign ((size_t) maxBlock * 2, 0.0f);
        high.assign ((size_t) maxBlock * 4, 0.0f);
    }

    void reset() noexcept { stages[0].reset(); stages[1].reset(); }

    /** Renvoie le tampon suréchantillonné (4n échantillons). */
    float* up (const float* in, int n) noexcept
    {
        stages[0].upsample (in, mid.data(), n);
        stages[1].upsample (mid.data(), high.data(), n * 2);
        return high.data();
    }

    /** Ramène le tampon suréchantillonné (rempli par up() puis traité) à n échantillons. */
    void down (float* out, int n) noexcept
    {
        stages[1].downsample (high.data(), mid.data(), n * 2);
        stages[0].downsample (mid.data(), out, n);
    }

private:
    HalfBandStage stages[2];
    std::vector<float> mid, high;
};

//==============================================================================
//  RÉVERBÉRATION
//  Algorithme Freeverb de Jezar at Dreampoint (domaine public) : 8 filtres en peigne
//  amortis en parallèle, puis 4 passe-tout en série, par canal. Code écrit pour JSG Guitar.
//==============================================================================
class Reverb
{
public:
    struct Parameters
    {
        float roomSize = 0.5f, damping = 0.5f, wetLevel = 0.33f, dryLevel = 0.4f, width = 1.0f, freezeMode = 0.0f;
    };

    Reverb() { setParameters (Parameters()); setSampleRate (44100.0); }

    /** Constantes de Freeverb : gain d'entrée, échelles du mouillé, du direct, de l'amortissement
        et de la taille de pièce. */
    static constexpr float inputGain = 0.015f, wetScale = 3.0f, dryScale = 2.0f;
    static constexpr float dampScale = 0.4f, roomScale = 0.28f, roomOffset = 0.7f;

    void setParameters (const Parameters& p) noexcept
    {
        const bool frozen = p.freezeMode >= 0.5f;
        const float wet = p.wetLevel * wetScale;

        setTarget (kWetSame,  wet * (0.5f + 0.5f * p.width));    // même canal
        setTarget (kWetCross, wet * (0.5f - 0.5f * p.width));    // canal opposé
        setTarget (kDry,      p.dryLevel * dryScale);
        setTarget (kDamp,     frozen ? 0.0f : p.damping * dampScale);
        setTarget (kFeedback, frozen ? 1.0f : p.roomSize * roomScale + roomOffset);
        gain = frozen ? 0.0f : inputGain;
    }

    void setSampleRate (double sampleRate)
    {
        // Longueurs d'origine (en échantillons à 44,1 kHz) ; le canal droit est décalé de 23
        static const int combLengths[numCombs] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int allpassLengths[numAllpasses] = { 556, 441, 341, 225 };
        const int spread = 23;
        const double scale = sampleRate / 44100.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < numCombs; ++i)     combs[ch][i].resize ((int) ((combLengths[i] + ch * spread) * scale));
            for (int i = 0; i < numAllpasses; ++i) allpasses[ch][i].resize ((int) ((allpassLengths[i] + ch * spread) * scale));
        }

        rampLength = jmax (1, (int) (0.01 * sampleRate));   // les réglages glissent en 10 ms
        current = targets;
        countdown.fill (0);
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            for (auto& c : combs[ch]) c.clear();
            for (auto& a : allpasses[ch]) a.clear();
        }
    }

    void processStereo (float* left, float* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            for (size_t t = 0; t < numTargets; ++t)
                if (countdown[t] > 0)
                    current[t] = --countdown[t] == 0 ? targets[t] : current[t] + steps[t];

            const float damp = current[kDamp], feedback = current[kFeedback];
            const float input = (left[i] + right[i]) * gain;
            float outL = 0.0f, outR = 0.0f;

            for (int j = 0; j < numCombs; ++j)
            {
                outL += combs[0][j].process (input, damp, feedback);
                outR += combs[1][j].process (input, damp, feedback);
            }

            for (int j = 0; j < numAllpasses; ++j)
            {
                outL = allpasses[0][j].process (outL);
                outR = allpasses[1][j].process (outR);
            }

            const float dry = current[kDry], same = current[kWetSame], cross = current[kWetCross];
            left[i]  = outL * same + outR * cross + left[i]  * dry;
            right[i] = outR * same + outL * cross + right[i] * dry;
        }
    }

private:
    /** Filtre en peigne avec un passe-bas (amortissement) dans la boucle. */
    struct Comb
    {
        std::vector<float> line;
        int pos = 0;
        float store = 0.0f;

        void resize (int length) { line.assign ((size_t) jmax (1, length), 0.0f); pos = 0; store = 0.0f; }
        void clear() noexcept    { std::fill (line.begin(), line.end(), 0.0f); store = 0.0f; }

        inline float process (float input, float damp, float feedback) noexcept
        {
            const float out = line[(size_t) pos];
            store = undenormalise (out * (1.0f - damp) + store * damp);
            line[(size_t) pos] = undenormalise (input + store * feedback);
            pos = pos + 1 < (int) line.size() ? pos + 1 : 0;
            return out;
        }
    };

    /** Passe-tout de Schroeder (gain 0,5). */
    struct Allpass
    {
        std::vector<float> line;
        int pos = 0;

        void resize (int length) { line.assign ((size_t) jmax (1, length), 0.0f); pos = 0; }
        void clear() noexcept    { std::fill (line.begin(), line.end(), 0.0f); }

        inline float process (float input) noexcept
        {
            const float delayed = line[(size_t) pos];
            line[(size_t) pos] = undenormalise (input + delayed * 0.5f);
            pos = pos + 1 < (int) line.size() ? pos + 1 : 0;
            return delayed - input;
        }
    };

    enum { numCombs = 8, numAllpasses = 4 };
    enum { kWetSame, kWetCross, kDry, kDamp, kFeedback, numTargets };

    /** Nouvelle valeur d'un réglage : rampe linéaire depuis la valeur actuelle. */
    void setTarget (int t, float value) noexcept
    {
        if (targets[(size_t) t] == value) return;
        targets[(size_t) t] = value;
        countdown[(size_t) t] = rampLength;
        steps[(size_t) t] = (value - current[(size_t) t]) / (float) rampLength;
    }

    Comb combs[2][numCombs];
    Allpass allpasses[2][numAllpasses];
    std::array<float, numTargets> targets {}, current {}, steps {};
    std::array<int, numTargets> countdown {};
    float gain = inputGain;
    int rampLength = 441;
};

//==============================================================================
/** FFT complexe radix 2 (taille fixe). */
class Fft
{
public:
    void prepare (int size)
    {
        n = size;
        cosTable.resize ((size_t) n / 2);
        sinTable.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
        {
            cosTable[(size_t) i] = (float) std::cos (twoPi * i / n);
            sinTable[(size_t) i] = (float) std::sin (twoPi * i / n);
        }
        bits = 0;
        while ((1 << bits) < n) ++bits;
        reversed.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed[(size_t) i] = r;
        }
    }

    /** Sur place ; inverse = vrai : transformée inverse non normalisée. */
    void perform (float* re, float* im, bool inverse) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = reversed[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }

        for (int size = 2; size <= n; size <<= 1)
        {
            const int half = size >> 1, step = n / size;
            for (int start = 0; start < n; start += size)
            {
                for (int k = 0; k < half; ++k)
                {
                    const float c = cosTable[(size_t) (k * step)];
                    const float s = inverse ? sinTable[(size_t) (k * step)] : -sinTable[(size_t) (k * step)];
                    const int a = start + k, b = a + half;
                    const float tr = re[b] * c - im[b] * s;
                    const float ti = re[b] * s + im[b] * c;
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
            }
        }
    }

private:
    int n = 0, bits = 0;
    std::vector<float> cosTable, sinTable;
    std::vector<int> reversed;
};

/** Convolution mono, partitionnée uniformément, sans latence : la taille de partition est celle
    du bloc audio (128 dans le navigateur, la période de la carte son sur PC). Pour un bloc B,
    chaque partition de l'IR fait B échantillons et la FFT N >= 2B. */
class Convolution
{
public:
    explicit Convolution (int blockSize = 128)
        : block (jmax (16, blockSize)), fftSize (nextPowerOfTwo (2 * jmax (16, blockSize))), bins (fftSize / 2 + 1)
    {
        fft.prepare (fftSize);
        re.assign ((size_t) fftSize, 0.0f);
        im.assign ((size_t) fftSize, 0.0f);
        history.assign ((size_t) fftSize, 0.0f);
    }

    int getBlockSize() const noexcept { return block; }

    /** Charge une IR déjà à la bonne fréquence d'échantillonnage : rognée (silence au début et à
        la fin) et normalisée. Fait des allocations : à appeler hors du fil audio. */
    void load (const float* ir, int length)
    {
        float peak = 0.0f;
        for (int i = 0; i < length; ++i) peak = jmax (peak, std::abs (ir[i]));
        if (peak <= 0.0f) { partitions = 0; return; }

        const float floor = peak * 1.0e-4f;    // -80 dB par rapport à la crête
        int start = 0, end = length;
        while (start < end && std::abs (ir[start]) < floor) ++start;
        while (end > start && std::abs (ir[end - 1]) < floor) --end;

        const int used = jmin (end - start, 48000 * 4);
        double energy = 0.0;
        for (int i = 0; i < used; ++i) energy += (double) ir[start + i] * ir[start + i];
        const float norm = energy > 0.0 ? (float) (0.125 / std::sqrt (energy)) : 0.0f;

        partitions = (used + block - 1) / block;
        irRe.assign ((size_t) partitions * bins, 0.0f);
        irIm.assign ((size_t) partitions * bins, 0.0f);

        for (int p = 0; p < partitions; ++p)
        {
            std::fill (re.begin(), re.end(), 0.0f);
            std::fill (im.begin(), im.end(), 0.0f);
            for (int i = 0; i < block; ++i)
            {
                const int at = p * block + i;
                if (at < used) re[(size_t) i] = ir[start + at] * norm;
            }
            fft.perform (re.data(), im.data(), false);
            std::copy_n (re.begin(), bins, irRe.begin() + p * bins);
            std::copy_n (im.begin(), bins, irIm.begin() + p * bins);
        }

        inRe.assign ((size_t) partitions * bins, 0.0f);
        inIm.assign ((size_t) partitions * bins, 0.0f);
        std::fill (history.begin(), history.end(), 0.0f);
        head = 0;
    }

    bool isLoaded() const noexcept { return partitions > 0; }

    void reset() noexcept
    {
        std::fill (inRe.begin(), inRe.end(), 0.0f);
        std::fill (inIm.begin(), inIm.end(), 0.0f);
        std::fill (history.begin(), history.end(), 0.0f);
    }

    /** Traite exactement getBlockSize() échantillons sur place. */
    void process (float* data) noexcept
    {
        if (partitions == 0) return;

        // Recouvrement-sauvegarde : les N derniers échantillons d'entrée, le bloc courant à la fin
        std::copy (history.begin() + block, history.end(), history.begin());
        std::copy_n (data, block, history.end() - block);
        std::copy (history.begin(), history.end(), re.begin());
        std::fill (im.begin(), im.end(), 0.0f);
        fft.perform (re.data(), im.data(), false);

        head = (head + partitions - 1) % partitions;
        std::copy_n (re.begin(), bins, inRe.begin() + head * bins);
        std::copy_n (im.begin(), bins, inIm.begin() + head * bins);

        // Somme des produits : partition p de l'IR avec le spectre d'entrée d'il y a p blocs
        std::fill (re.begin(), re.end(), 0.0f);
        std::fill (im.begin(), im.end(), 0.0f);

        for (int p = 0; p < partitions; ++p)
        {
            const int slot = (head + p) % partitions;
            const float* xr = inRe.data() + slot * bins;
            const float* xi = inIm.data() + slot * bins;
            const float* hr = irRe.data() + p * bins;
            const float* hi = irIm.data() + p * bins;
            for (int k = 0; k < bins; ++k)
            {
                re[(size_t) k] += xr[k] * hr[k] - xi[k] * hi[k];
                im[(size_t) k] += xr[k] * hi[k] + xi[k] * hr[k];
            }
        }

        // Spectre hermitien complet, puis retour au temps
        for (int k = 1; k < bins - 1; ++k)
        {
            re[(size_t) (fftSize - k)] = re[(size_t) k];
            im[(size_t) (fftSize - k)] = -im[(size_t) k];
        }
        fft.perform (re.data(), im.data(), true);

        const float scale = 1.0f / (float) fftSize;
        for (int i = 0; i < block; ++i)
            data[i] = re[(size_t) (fftSize - block + i)] * scale;
    }

private:
    int block, fftSize, bins;
    Fft fft;
    std::vector<float> re, im, history, irRe, irIm, inRe, inIm;
    int partitions = 0, head = 0;
};

} // namespace jsg
