// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Réglages de JSG Guitar vus par le logiciel hôte (automation, sauvegarde du projet).
    Même ordre, mêmes bornes et mêmes valeurs par défaut que l'interface (ui/js/data.js)
    et le moteur (moteur/api.cpp) : ne pas modifier l'ordre.

    Valeur « normalisée » (0..1, celle du logiciel hôte) <-> valeur réelle : même échelle que les
    potards de l'interface (échelle « skew » : la valeur centre tombe à mi-course).
*/

#include <cmath>
#include <cstdio>
#include <string>

namespace jsg::plugin
{

enum class Kind { toggle, choice, knob, ranged, position };

struct ParamInfo
{
    const char* id;
    const char* name;
    Kind kind;
    float min, max, def, centre;
    const char* unit;
    const char* const* choices;     // pour Kind::choice
    int numChoices;
};

inline constexpr const char* channels[] = { "Clair", "Crunch", "Lead" };
inline constexpr const char* cabinets[] = { "JSG 4x12 Modern", "JSG 4x12 Vintage", "JSG 2x12 Alnico", "JSG 1x12 Open Back", "JSG 4x10 Blues" };
inline constexpr const char* bassCabinets[] = { "JSG 8x10", "JSG 4x10 Basse", "JSG 4x10 + tweeter", "JSG 1x15", "JSG 2x12 Basse" };
inline constexpr const char* mics[] = { "Dynamique", "Ruban", "Dyn. + Ruban", "Dyn. large", "Statique" };
inline constexpr const char* rooms[] = { "Studio A", "Studio B", "Live Room", "Grande salle", "Cave" };
inline constexpr const char* driveTypes[] = { "MD", "OD", "DIST", "FUZZ" };
inline constexpr const char* instruments[] = { "Guitare", "Basse" };
inline constexpr const char* strings[] = { "4 cordes", "5 cordes", "6 cordes" };

constexpr ParamInfo toggle (const char* id, const char* name, bool def)   { return { id, name, Kind::toggle, 0, 1, def ? 1.0f : 0.0f, 0, "", nullptr, 0 }; }
constexpr ParamInfo knob (const char* id, const char* name, float def)    { return { id, name, Kind::knob, 0, 10, def, 0, "", nullptr, 0 }; }
constexpr ParamInfo ranged (const char* id, const char* name, float min, float max, float def, const char* unit, float centre = 0)
                                                                         { return { id, name, Kind::ranged, min, max, def, centre, unit, nullptr, 0 }; }
template <int N>
constexpr ParamInfo choice (const char* id, const char* name, const char* const (&list)[N], int def)
                                                                         { return { id, name, Kind::choice, 0, (float) (N - 1), (float) def, 0, "", list, N }; }

inline constexpr ParamInfo fixedParams[] = {
    toggle ("power", "Power", true),
    toggle ("mute", "Muet", false),
    choice ("channel", "Canal", channels, 2),
    toggle ("bright", "Bright", false),
    ranged ("input", "Entrée", -24, 24, 0, "dB"),
    knob ("gain", "Gain", 5),
    knob ("bass", "Bass", 5),
    knob ("mid", "Middle", 5),
    knob ("treble", "Treble", 5),
    knob ("presence", "Presence", 5),
    knob ("master", "Master", 6),

    toggle ("cabOn", "Baffle", true),
    choice ("cabType", "Type de baffle", cabinets, 0),
    toggle ("micOn", "Micro", true),
    choice ("micType", "Type de micro", mics, 2),
    toggle ("roomOn", "Room", true),
    choice ("roomType", "Type de room", rooms, 0),
    knob ("roomLevel", "Niveau de la room", 5),
    ranged ("lowCut", "Coupe-bas", 20, 300, 60, "Hz", 100),
    ranged ("highCut", "Coupe-haut", 2000, 20000, 12000, "Hz", 7000),
    toggle ("irOn", "IR Loader", false),

    toggle ("eqOn", "EQ", false),
    ranged ("eq1", "EQ 100 Hz", -12, 12, 0, "dB"),
    ranged ("eq2", "EQ 250 Hz", -12, 12, 0, "dB"),
    ranged ("eq3", "EQ 800 Hz", -12, 12, 0, "dB"),
    ranged ("eq4", "EQ 2,5 kHz", -12, 12, 0, "dB"),
    ranged ("eq5", "EQ 6 kHz", -12, 12, 0, "dB"),

    ranged ("output", "Sortie", -24, 12, 0, "dB"),

    toggle ("gateOn", "Noise gate", false),
    ranged ("gateThreshold", "Seuil du noise gate", -80, -20, -55, "dB"),
    ranged ("gateDecay", "Fermeture du noise gate", 10, 1000, 80, "ms", 100),
    toggle ("compOn", "Compresseur", false),
    knob ("compLevel", "Niveau du compresseur", 5),
    ranged ("compAttack", "Attaque du compresseur", 1, 100, 10, "ms", 10),
    toggle ("driveOn", "Drive : marche", false),
    choice ("driveType", "Type de drive", driveTypes, 0),
    knob ("drive", "Drive", 4),
    toggle ("fxOn", "Effets", false),
    toggle ("fxChorus", "Chorus", false),
    toggle ("fxDelay", "Delay", false),
    toggle ("fxReverb", "Reverb", true),
    knob ("fxMix", "Mix du chorus", 5),
    ranged ("fxTime", "Temps du delay", 40, 1200, 380, "ms", 300),
    knob ("fxFeedback", "Répétitions du delay", 3.5f),
    knob ("fxLevel", "Niveau delay et reverb", 3),

    choice ("instrument", "Instrument", instruments, 0),
    choice ("bassCab", "Baffle basse", bassCabinets, 0),
    knob ("diBlend", "DI", 4),
    choice ("bassStrings", "Cordes", strings, 0),
};

inline constexpr int numFixedParams = (int) (sizeof fixedParams / sizeof fixedParams[0]);   // 49
inline constexpr int numSlots = 16, paramsPerSlot = 5;     // par emplacement : marche/arrêt + 4 potards (position 0..1)
inline constexpr int numParams = numFixedParams + numSlots * paramsPerSlot;
inline constexpr int bypassId = 1000;
static_assert (numFixedParams == 49, "l'ordre des réglages doit rester celui du moteur");

/** Description d'un réglage (fixes, puis ceux des 16 emplacements de pédales). */
inline ParamInfo paramInfo (int index, std::string& nameStorage)
{
    if (index < numFixedParams) { nameStorage = fixedParams[index].name; return fixedParams[index]; }
    const int slot = (index - numFixedParams) / paramsPerSlot, k = (index - numFixedParams) % paramsPerSlot;
    nameStorage = "Pédale " + std::to_string (slot + 1) + (k == 0 ? std::string (" : marche") : " : potard " + std::to_string (k));
    if (k == 0) return { "slotOn", nullptr, Kind::toggle, 0, 1, 0, 0, "", nullptr, 0 };
    return { "slotKnob", nullptr, Kind::position, 0, 1, 0.5f, 0, "", nullptr, 0 };
}

inline float skewOf (const ParamInfo& p)
{
    if (p.kind == Kind::ranged && p.centre > p.min && p.centre < p.max)
        return (float) (std::log (0.5) / std::log ((p.centre - p.min) / (p.max - p.min)));
    return 1.0f;
}

inline double toNormalized (const ParamInfo& p, double v)
{
    double x = (v - p.min) / (p.max - p.min);
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    const float s = skewOf (p);
    return s == 1.0f ? x : std::pow (x, (double) s);
}

inline double toPlain (const ParamInfo& p, double n)
{
    n = n < 0 ? 0 : (n > 1 ? 1 : n);
    if (p.kind == Kind::toggle || p.kind == Kind::choice) return std::round (p.min + n * (p.max - p.min));
    const float s = skewOf (p);
    return p.min + (p.max - p.min) * (s == 1.0f || n <= 0 ? n : std::exp (std::log (n) / s));
}

inline std::string formatValue (const ParamInfo& p, double v)
{
    char b[64];
    switch (p.kind)
    {
        case Kind::toggle:   return v > 0.5 ? "ON" : "OFF";
        case Kind::choice:   { const int i = (int) std::lround (v); return i >= 0 && i < p.numChoices ? p.choices[i] : ""; }
        case Kind::knob:     std::snprintf (b, sizeof b, "%.1f", v); break;
        case Kind::position: std::snprintf (b, sizeof b, "%.0f %%", v * 100.0); break;
        case Kind::ranged:
        {
            const bool whole = std::string (p.unit) == "Hz" || std::string (p.unit) == "ms";
            std::snprintf (b, sizeof b, whole ? "%.0f %s" : "%.1f %s", v, p.unit);
            break;
        }
    }
    std::string s = b;
    for (auto& c : s) if (c == '.') c = ',';
    return s;
}

} // namespace jsg::plugin
