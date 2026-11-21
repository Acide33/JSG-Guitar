// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Étirement du temps pour le lecteur de morceau (ralentir ou accélérer sans changer la hauteur ;
    combiné à une relecture plus rapide ou plus lente, il change aussi la tonalité).
    Portage sans JUCE de Source/TimeStretcher.h : même méthode (vocodeur de phase à verrouillage
    de phase, fenêtres de 4096, recouvrement 75 %, même rotation pour les deux canaux).

    Les deux canaux passent dans une seule FFT complexe (gauche en partie réelle, droite en partie
    imaginaire), puis sont séparés : deux fois moins de calcul que deux FFT réelles.
    Aucune allocation après la construction : utilisable dans le fil audio.
*/

#include "dsp.h"

#include <cstdint>

namespace jsg
{

class TimeStretcher
{
public:
    static constexpr int size    = 4096;
    static constexpr int hop     = size / 4;        // pas de sortie
    static constexpr int numBins = size / 2 + 1;

    TimeStretcher()
    {
        fft.prepare (size);
        window.resize ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (twoPi * ((double) i + 0.5) / (double) size);

        for (auto* v : { &inL, &inR, &zRe, &zIm, &accL, &accR }) v->assign ((size_t) size, 0.0f);
        for (auto* v : { &lRe, &lIm, &rRe, &rIm }) v->assign ((size_t) numBins, 0.0f);
        for (auto* v : { &outL, &outR }) v->assign ((size_t) hop, 0.0f);
        for (auto* v : { &magnitude, &phase, &lastPhase, &synthPhase, &rotation }) v->assign ((size_t) numBins, 0.0f);
    }

    /** Repart de zéro à cette position du morceau (en échantillons). */
    void reset (double sourcePosition) noexcept
    {
        position = sourcePosition;
        heard = sourcePosition;
        first = true;
        preRoll = 3;
        available = 0;
        readIndex = 0;
        std::fill (accL.begin(), accL.end(), 0.0f);
        std::fill (accR.begin(), accR.end(), 0.0f);
    }

    /** Position du morceau qu'on est en train d'entendre (en échantillons). */
    double getPosition() const noexcept     { return heard; }

    /** Ramène la position dans le morceau quand il boucle. */
    void wrap (double length) noexcept
    {
        if (position >= length)
        {
            position -= length;
            lastStart -= (int64_t) length;
        }
    }

    /** Position de la prochaine fenêtre à lire : sert à détecter la fin du morceau. */
    double getReadPosition() const noexcept { return position; }

    /** Donne l'échantillon étiré suivant. `stretch` : durée multipliée par ce facteur (2 = deux fois
        plus lent). `fetch (début, gauche, droite, nombre)` fournit le morceau à partir d'une position. */
    template <typename Fetch>
    void pull (float& left, float& right, double stretch, Fetch&& fetch) noexcept
    {
        while (readIndex >= available)
            synthesise (stretch, fetch);

        left = outL[(size_t) readIndex];
        right = outR[(size_t) readIndex];
        ++readIndex;
    }

private:
    template <typename Fetch>
    void synthesise (double stretch, Fetch&& fetch) noexcept
    {
        const double analysisHop = (double) hop / jlimit (0.25, 4.0, stretch);

        if (first)   // après un départ : trois fenêtres d'avance, pour que le son démarre à plein volume
            position -= 3.0 * analysisHop;

        const auto centre = (int64_t) std::floor (position);
        const auto start = centre - size / 2;
        fetch (start, inL.data(), inR.data(), size);

        // Les deux canaux fenêtrés dans une seule FFT complexe
        for (int i = 0; i < size; ++i)
        {
            zRe[(size_t) i] = inL[(size_t) i] * window[(size_t) i];
            zIm[(size_t) i] = inR[(size_t) i] * window[(size_t) i];
        }
        fft.perform (zRe.data(), zIm.data(), false);

        // Séparation : G[k] = (Z[k] + conj Z[N-k]) / 2 ; D[k] = (Z[k] - conj Z[N-k]) / 2i
        for (int b = 0; b < numBins; ++b)
        {
            const int m = (size - b) & (size - 1);
            const float ar = zRe[(size_t) b], ai = zIm[(size_t) b];
            const float br = zRe[(size_t) m], bi = -zIm[(size_t) m];   // conj Z[N-k]
            lRe[(size_t) b] = 0.5f * (ar + br);   lIm[(size_t) b] = 0.5f * (ai + bi);
            rRe[(size_t) b] = 0.5f * (ai - bi);   rIm[(size_t) b] = -0.5f * (ar - br);
        }

        // Module et phase de la somme des deux canaux : c'est elle qui guide la rotation
        for (int b = 0; b < numBins; ++b)
        {
            const float re = lRe[(size_t) b] + rRe[(size_t) b];
            const float im = lIm[(size_t) b] + rIm[(size_t) b];
            magnitude[(size_t) b] = re * re + im * im;
            phase[(size_t) b] = std::atan2 (im, re);
        }

        if (first)
        {
            std::copy (phase.begin(), phase.end(), synthPhase.begin());
            std::fill (rotation.begin(), rotation.end(), 0.0f);
        }
        else
        {
            const float actualHop = (float) (centre - (lastStart + size / 2));
            const float tau = (float) twoPi;

            const auto advance = [&] (int b)
            {
                const float expected = tau * (float) b / (float) size;
                float deviation = phase[(size_t) b] - lastPhase[(size_t) b] - expected * actualHop;
                deviation -= tau * std::round (deviation / tau);
                const float frequency = actualHop > 0.5f ? expected + deviation / actualHop : expected;
                const float next = synthPhase[(size_t) b] + frequency * (float) hop;
                return next - tau * std::round (next / tau);
            };

            int regionStart = 0, previousPeak = -1;

            const auto closeRegion = [&] (int peak, int regionEnd)
            {
                const float turn = advance (peak) - phase[(size_t) peak];
                for (int b = regionStart; b < regionEnd; ++b)
                    rotation[(size_t) b] = turn;
                regionStart = regionEnd;
            };

            for (int b = 2; b < numBins - 2; ++b)
            {
                const float m = magnitude[(size_t) b];
                const bool isPeak = m > magnitude[(size_t) (b - 1)] && m > magnitude[(size_t) (b - 2)]
                                 && m >= magnitude[(size_t) (b + 1)] && m >= magnitude[(size_t) (b + 2)];
                if (! isPeak) continue;

                if (previousPeak >= 0)
                {
                    int valley = previousPeak + 1;
                    for (int k = previousPeak + 1; k < b; ++k)
                        if (magnitude[(size_t) k] < magnitude[(size_t) valley]) valley = k;
                    closeRegion (previousPeak, valley);
                }
                previousPeak = b;
            }

            if (previousPeak >= 0) closeRegion (previousPeak, numBins);
            else                   std::fill (rotation.begin(), rotation.end(), 0.0f);

            for (int b = 0; b < numBins; ++b)
            {
                const float next = phase[(size_t) b] + rotation[(size_t) b];
                synthPhase[(size_t) b] = next - tau * std::round (next / tau);
            }
        }

        std::copy (phase.begin(), phase.end(), lastPhase.begin());
        lastStart = start;

        // Même rotation pour les deux canaux, puis spectre complet Z = G + i·D et retour au signal
        for (int b = 0; b < numBins; ++b)
        {
            const float c = std::cos (rotation[(size_t) b]), s = std::sin (rotation[(size_t) b]);
            float gr = lRe[(size_t) b] * c - lIm[(size_t) b] * s, gi = lRe[(size_t) b] * s + lIm[(size_t) b] * c;
            float dr = rRe[(size_t) b] * c - rIm[(size_t) b] * s, di = rRe[(size_t) b] * s + rIm[(size_t) b] * c;
            if (b == 0 || b == numBins - 1) { gi = 0.0f; di = 0.0f; }   // composantes réelles

            zRe[(size_t) b] = gr - di;   zIm[(size_t) b] = gi + dr;          // Z[k] = G[k] + i D[k]
            if (b > 0 && b < numBins - 1)
            {
                const int m = size - b;                                      // Z[N-k] = conj G[k] + i conj D[k]
                zRe[(size_t) m] = gr + di;   zIm[(size_t) m] = -gi + dr;
            }
        }
        fft.perform (zRe.data(), zIm.data(), true);

        // Hann à l'analyse et à la synthèse, recouvrement de 75 % : la somme vaut 1,5 ; FFT inverse : 1/N
        const float gain = 1.0f / (1.5f * (float) size);
        for (int i = 0; i < size; ++i)
        {
            accL[(size_t) i] += zRe[(size_t) i] * window[(size_t) i] * gain;
            accR[(size_t) i] += zIm[(size_t) i] * window[(size_t) i] * gain;
        }

        std::copy (accL.begin(), accL.begin() + hop, outL.begin());
        std::copy (accR.begin(), accR.begin() + hop, outR.begin());

        for (auto* acc : { &accL, &accR })
        {
            std::copy (acc->begin() + hop, acc->end(), acc->begin());
            std::fill (acc->end() - hop, acc->end(), 0.0f);
        }

        heard = jmax (0.0, position - 1.5 * analysisHop);
        position += analysisHop;
        first = false;

        if (preRoll > 0) { --preRoll; available = 0; }
        else             available = hop;

        readIndex = 0;
    }

    Fft fft;
    std::vector<float> window, inL, inR, zRe, zIm, lRe, lIm, rRe, rIm, accL, accR, outL, outR;
    std::vector<float> magnitude, phase, lastPhase, synthPhase, rotation;

    double position = 0.0, heard = 0.0;
    int64_t lastStart = 0;
    bool first = true;
    int preRoll = 3, available = 0, readIndex = 0;
};

} // namespace jsg
