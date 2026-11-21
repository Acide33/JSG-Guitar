// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Lecteur de morceau de la version PC — même fonctionnement que Source/BackingPlayer.h
    (un seul jeu de pistes : le morceau entier) :
      - à 100 % et 0 demi-ton, lecture directe avec interpolation à 4 points (le morceau n'a pas
        forcément la fréquence de la carte son) ;
      - sinon, étirement du temps (stretcher.h) puis relecture plus ou moins rapide :
        la tonalité change sans changer la vitesse, et inversement ;
      - boucle du morceau, boucle A-B, fondus de 8 ms pour les sauts et les changements de mode.

    Le morceau chargé est confié au fil audio par un pointeur atomique ; l'ancien est rendu au fil
    de l'interface (collectGarbage) : aucune allocation ni libération dans le fil audio.
*/

#include "stretcher.h"

#include <atomic>
#include <memory>
#include <string>

namespace jsg
{

struct Song
{
    std::string title;
    double sampleRate = 44100.0;
    int64_t length = 0;
    std::vector<float> left, right;
    std::vector<float> overview;   // crêtes, pour la forme d'onde de l'interface
};

class Player
{
public:
    static constexpr float minSpeed = 0.5f, maxSpeed = 1.5f;
    static constexpr int maxTranspose = 12;

    ~Player() { delete current; delete pending.exchange (nullptr); delete retired.exchange (nullptr); }

    //==========================================================================
    // Fil de l'interface
    /** Confie un nouveau morceau au lecteur (l'arrêt, et la position revient au début). */
    void setSong (std::unique_ptr<Song> song)
    {
        playing.store (false);
        clearLoopAB();
        lengthSeconds.store (song != nullptr ? (double) song->length / song->sampleRate : 0.0);
        position.store (0.0);
        seekRequest.store (0.0);
        delete pending.exchange (song.release());
    }

    void collectGarbage()                       { delete retired.exchange (nullptr); }

    void play()                                 { if (lengthSeconds.load() > 0.0) playing.store (true); }
    void pause()                                { playing.store (false); }
    void stop()                                 { playing.store (false); seek (isLoopABActive() ? abStart.load() : 0.0); }
    void seek (double seconds)                  { seekRequest.store (jlimit (0.0, jmax (0.0, lengthSeconds.load() - 0.01), seconds)); }
    void setLooping (bool on)                   { looping.store (on); }
    void setVolume (float v)                    { volume.store (jlimit (0.0f, 1.5f, v)); }
    void setSpeed (float s)                     { speed.store (jlimit (minSpeed, maxSpeed, s)); }
    void setTranspose (int semitones)           { transpose.store (jlimit (-maxTranspose, maxTranspose, semitones)); }

    /** Boucle A-B (en secondes ; une valeur négative efface le point). */
    void setLoopAB (double a, double b, bool on)
    {
        if (a >= 0.0 && b >= 0.0 && b < a) std::swap (a, b);
        if (a >= 0.0 && b >= 0.0 && b - a < 0.1) b = a + 0.1;
        abStart.store (a); abEnd.store (b); abOn.store (on);
    }
    void clearLoopAB()                          { abOn.store (false); abStart.store (-1.0); abEnd.store (-1.0); }

    bool   isPlaying() const noexcept           { return playing.load(); }
    double getPosition() const noexcept         { return position.load(); }
    double getLength() const noexcept           { return lengthSeconds.load(); }
    bool   isLoopABActive() const noexcept      { return abOn.load() && abStart.load() >= 0.0 && abEnd.load() > abStart.load(); }

    //==========================================================================
    // Fil audio
    void prepare (double sampleRate) noexcept
    {
        hostRate = sampleRate;
        fadeStep = 1.0f / (float) (sampleRate * 0.008);
    }

    /** Ajoute le morceau à la sortie. */
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (retired.load() == nullptr)
            if (auto* next = pending.exchange (nullptr))
            {
                retired.store (current);
                current = next;
                pos = 0.0; fade = 0.0f; stretching = false;
            }

        const Song* s = current;
        if (s == nullptr || s->length < 4)
            return;

        const bool wantPlay = playing.load();
        double seekTo = seekRequest.load();
        const int64_t last = s->length - 1;

        const double speedNow = (double) speed.load();
        const double pitchRatio = std::pow (2.0, (double) transpose.load() / 12.0);
        const bool wantStretch = std::abs (speedNow - 1.0) > 1.0e-3 || transpose.load() != 0;

        const auto jumpTo = [&] (double songSamples)
        {
            pos = jlimit (0.0, (double) last, songSamples);
            if (stretching) restartStretcher();
        };

        const auto switchMode = [&]
        {
            if (! stretching) { stretching = true; restartStretcher(); }
            else              { pos = jlimit (0.0, (double) last, stretcher.getPosition()); stretching = false; }
        };

        if (! wantPlay && fade <= 0.0f)
        {
            if (seekTo >= 0.0)
            {
                jumpTo (seekTo * s->sampleRate);
                seekRequest.compare_exchange_strong (seekTo, -1.0);
                position.store (pos / s->sampleRate);
            }
            if (wantStretch != stretching) switchMode();
            return;
        }

        const double ratio = s->sampleRate / hostRate;
        const bool abActive = isLoopABActive();
        const double abA = abStart.load(), abEndSample = abEnd.load() * s->sampleRate;
        const float level = volume.load();
        const bool loop = looping.load();
        const float* sl = s->left.data();
        const float* sr = s->right.data();

        const auto fetch = [&] (int64_t start, float* l, float* r, int count)
        {
            for (int n = 0; n < count; ++n)
            {
                auto p = start + n;
                if (loop)                       p = ((p % s->length) + s->length) % s->length;
                else if (p < 0 || p > last)     { l[n] = r[n] = 0.0f; continue; }
                l[n] = sl[p];
                r[n] = sr[p];
            }
        };

        for (int n = 0; n < numSamples; ++n)
        {
            const float fadeTarget = (wantPlay && seekTo < 0.0 && wantStretch == stretching) ? 1.0f : 0.0f;
            if (fade < fadeTarget)      fade = jmin (fadeTarget, fade + fadeStep);
            else if (fade > fadeTarget) fade = jmax (fadeTarget, fade - fadeStep);

            if (fade <= 0.0f)
            {
                if (seekTo >= 0.0)
                {
                    jumpTo (seekTo * s->sampleRate);
                    seekRequest.compare_exchange_strong (seekTo, -1.0);
                    seekTo = seekRequest.load();
                    continue;
                }
                if (wantStretch != stretching) { switchMode(); continue; }
                if (! wantPlay) break;
            }

            float sumL = 0.0f, sumR = 0.0f;
            bool ended = false;

            if (! stretching)
            {
                const auto i1 = (int64_t) pos;
                const float t = (float) (pos - (double) i1);
                const auto i0 = jmax ((int64_t) 0, i1 - 1);
                const auto i2 = jmin (last, i1 + 1), i3 = jmin (last, i1 + 2);
                float w[4];
                hermite (t, w);
                sumL = w[0] * sl[i0] + w[1] * sl[i1] + w[2] * sl[i2] + w[3] * sl[i3];
                sumR = w[0] * sr[i0] + w[1] * sr[i1] + w[2] * sr[i2] + w[3] * sr[i3];

                pos += ratio;

                if (abActive && seekTo < 0.0 && pos >= abEndSample)
                {
                    seekRequest.store (abA);
                    seekTo = abA;
                }

                if (pos >= (double) last)
                {
                    pos = 0.0;
                    ended = ! loop;
                }
            }
            else
            {
                const double stretch = pitchRatio / speedNow;
                fraction += pitchRatio * ratio;

                while (fraction >= 1.0)
                {
                    fraction -= 1.0;
                    for (int k = 0; k < 3; ++k) { historyL[k] = historyL[k + 1]; historyR[k] = historyR[k + 1]; }
                    stretcher.pull (historyL[3], historyR[3], stretch, fetch);

                    if (stretcher.getReadPosition() >= (double) s->length)
                    {
                        if (loop) stretcher.wrap ((double) s->length);
                        else      ended = true;
                    }
                }

                float w[4];
                hermite ((float) fraction, w);
                sumL = w[0] * historyL[0] + w[1] * historyL[1] + w[2] * historyL[2] + w[3] * historyL[3];
                sumR = w[0] * historyR[0] + w[1] * historyR[1] + w[2] * historyR[2] + w[3] * historyR[3];
                pos = jlimit (0.0, (double) last, stretcher.getPosition());

                if (abActive && seekTo < 0.0 && pos >= abEndSample)
                {
                    seekRequest.store (abA);
                    seekTo = abA;
                }
            }

            const float k = level * fade;
            left[n]  = softLimit (left[n] + sumL * k);
            right[n] = softLimit (right[n] + sumR * k);

            if (ended)
            {
                playing.store (false);
                endedFlag.store (true);
                fade = 0.0f;
                pos = 0.0;
                if (stretching) restartStretcher();
                break;
            }
        }

        position.store (pos / s->sampleRate);
    }

    /** Vrai une fois après la fin du morceau (lu par l'interface). */
    bool takeEnded() noexcept { return endedFlag.exchange (false); }

private:
    static inline void hermite (float t, float* w) noexcept
    {
        w[0] = t * (-0.5f + t * (1.0f - 0.5f * t));
        w[1] = 1.0f + t * t * (-2.5f + 1.5f * t);
        w[2] = t * (0.5f + t * (2.0f - 1.5f * t));
        w[3] = t * t * (-0.5f + 0.5f * t);
    }

    static inline float softLimit (float x) noexcept
    {
        const float a = std::abs (x);
        if (a <= 0.7f) return x;
        const float y = 0.7f + 0.3f * std::tanh ((a - 0.7f) / 0.3f);
        return x < 0.0f ? -y : y;
    }

    void restartStretcher() noexcept
    {
        stretcher.reset (pos);
        fraction = 0.0;
        for (int k = 0; k < 4; ++k) { historyL[k] = 0.0f; historyR[k] = 0.0f; }
    }

    Song* current = nullptr;
    std::atomic<Song*> pending { nullptr }, retired { nullptr };

    std::atomic<bool> playing { false }, looping { false }, abOn { false }, endedFlag { false };
    std::atomic<double> seekRequest { -1.0 }, position { 0.0 }, lengthSeconds { 0.0 };
    std::atomic<double> abStart { -1.0 }, abEnd { -1.0 };
    std::atomic<float> volume { 0.8f }, speed { 1.0f };
    std::atomic<int> transpose { 0 };

    double hostRate = 48000.0, pos = 0.0, fraction = 0.0;
    float fade = 0.0f, fadeStep = 0.001f;
    float historyL[4] {}, historyR[4] {};
    TimeStretcher stretcher;
    bool stretching = false;
};

} // namespace jsg
