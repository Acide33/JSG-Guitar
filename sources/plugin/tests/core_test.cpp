// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
// Test du cœur du plug-in : blocs de tailles variées, sauvegarde / rappel, IR
#include "plugin_core.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <cstring>
using namespace jsg::plugin;

static std::vector<float> guitar (int n, double rate)
{
    // Corde pincée (Karplus-Strong), quelques notes
    std::vector<float> out ((size_t) n, 0.0f);
    std::minstd_rand rng (1);
    std::uniform_real_distribution<float> u (-1, 1);
    const double freqs[] = { 82.4, 110.0, 146.8, 196.0 };
    for (int note = 0; note < 4; ++note)
    {
        const int start = note * n / 4, period = (int) (rate / freqs[note]);
        std::vector<float> line ((size_t) period);
        for (auto& x : line) x = u (rng) * 0.4f;
        for (int i = start, k = 0; i < n; ++i, ++k)
        {
            const size_t a = (size_t) (k % period), b = (size_t) ((k + 1) % period);
            const float y = line[a];
            line[a] = 0.996f * 0.5f * (line[a] + line[b]);
            out[(size_t) i] += y;
        }
    }
    return out;
}

static std::vector<float> run (Core& core, const std::vector<float>& in, std::vector<int> blocks)
{
    std::vector<float> out (in.size() * 2);
    std::vector<float> l (4096), r (4096);
    size_t pos = 0;
    int bi = 0;
    while (pos < in.size())
    {
        const int n = (int) std::min<size_t> ((size_t) blocks[(size_t) (bi++ % (int) blocks.size())], in.size() - pos);
        const float* ins[1] = { in.data() + pos };
        float* outs[2] = { l.data(), r.data() };
        core.process (ins, 1, outs, 2, n);
        for (int i = 0; i < n; ++i) { out[(pos + (size_t) i) * 2] = l[(size_t) i]; out[(pos + (size_t) i) * 2 + 1] = r[(size_t) i]; }
        pos += (size_t) n;
    }
    return out;
}

static double rms (const std::vector<float>& v) { double s = 0; for (float x : v) s += (double) x * x; return std::sqrt (s / (double) v.size()); }
static double maxDiff (const std::vector<float>& a, const std::vector<float>& b) { double m = 0; for (size_t i = 0; i < a.size(); ++i) m = std::max (m, (double) std::abs (a[i] - b[i])); return m; }
static bool finite (const std::vector<float>& v) { for (float x : v) if (! std::isfinite (x)) return false; return true; }

int main()
{
    const double rate = 48000;
    const auto in = guitar ((int) rate * 3, rate);
    int failures = 0;
    const auto check = [&] (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "OK   " : "ÉCHEC", what); if (! ok) ++failures; };

    Core a;  a.prepare (rate);
    Core b;  b.prepare (rate);
    const auto outA = run (a, in, { 512 });
    const auto outB = run (b, in, { 1, 7, 64, 333, 1024, 17, 128, 2048, 5 });
    std::printf ("rms entrée %.4f, sortie %.4f\n", rms (in), rms (outA));
    check (finite (outA) && finite (outB), "pas de valeur invalide");
    check (rms (outA) > 0.01, "le son sort");
    check (maxDiff (outA, outB) < 1e-6, "même son quelle que soit la taille des blocs du logiciel hôte");

    // Latence : rien avant 32 échantillons (+ latence du moteur)
    double early = 0; for (int i = 0; i < 32 * 2; ++i) early = std::max (early, (double) std::abs (outA[(size_t) i]));
    check (early == 0.0, "latence annoncée de 32 échantillons respectée");

    // Réglages : gain et pédale, puis sauvegarde / rappel dans une autre instance
    Core c;  c.prepare (rate);
    c.setFromHost (5, 9.0f);     // gain
    c.setFromHost (2, 0.0f);     // canal clair
    const std::string state = c.getState();
    Core d;  d.prepare (rate);
    check (d.setState (state), "état relu");
    const auto outC = run (c, in, { 256 });
    const auto outD = run (d, in, { 256 });
    check (maxDiff (outC, outD) < 1e-6, "le projet rappelé sonne pareil");
    check (maxDiff (outA, outC) > 1e-3, "les réglages changent le son");

    // Contourner : entrée retardée de 32 échantillons
    Core e;  e.prepare (rate);  e.setBypass (true);
    const auto outE = run (e, in, { 100 });
    double bd = 0; for (size_t i = 32; i < in.size(); ++i) bd = std::max (bd, (double) std::abs (outE[i * 2] - in[i - 32]));
    check (bd < 1e-7, "contourné : le son passe tel quel (32 échantillons de retard)");

    // Rappel d'un projet à une autre fréquence
    Core f;  f.setState (state);  f.prepare (44100);
    const auto outF = run (f, in, { 441 });
    check (finite (outF) && rms (outF) > 0.01, "rappel puis fréquence 44,1 kHz");

    std::printf ("taille de l'état : %zu octets\n", state.size());
    return failures;
}
