// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Moteur audio de JSG Guitar — copie de Source/AmpEngine.h pour la version navigateur.

    La chaîne de traitement, les modèles (baffles, micros, rooms), les canaux de l'ampli,
    le mélange DI de la basse et les réglages sont ceux du plug-in, à l'identique.
    Différences : pas de verrous (dans le navigateur, un seul fil fait tout le travail audio),
    et les outils de JUCE sont remplacés par ceux de dsp.h.
*/

#include "pedals.h"

namespace jsg
{

struct SlotState
{
    bool on = false;
    std::array<float, maxPedalKnobs> knobs { 0.5f, 0.5f, 0.5f, 0.5f };
};

struct RackState
{
    bool  gateOn = false;    float gateThresholdDb = -55.0f, gateDecayMs = 80.0f;
    bool  compOn = false;    float compLevel = 5.0f, compAttackMs = 10.0f;
    bool  driveOn = false;   int   driveType = 0;   float drive = 4.0f;
    bool  fxOn = false;
    bool  chorusOn = false, delayOn = false, reverbOn = true;
    float fxMix = 5.0f;
    float fxTimeMs = 380.0f, fxFeedback = 3.5f;
    float fxLevel = 3.0f;
};

struct EngineParams
{
    bool  power = true;
    bool  mute  = false;
    bool  bassGuitar = false;
    float diBlend = 0.4f;
    int   channel = 2;
    bool  bright = false;
    float inputDb = 0.0f;
    float gain = 0.5f, bass = 0.5f, mid = 0.5f, treble = 0.5f, presence = 0.5f, master = 0.6f;
    bool  cabOn = true;   int cabType = 0;   int bassCabType = 0;
    bool  micOn = true;   int micType = 2;
    bool  roomOn = true;  int roomType = 0;
    float roomLevel = 0.5f;
    float lowCutHz = 60.0f, highCutHz = 12000.0f;
    bool  irOn = false;
    bool  eqOn = false;
    std::array<float, 5> eqDb { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float outputDb = 0.0f;
    RackState rack;
    std::array<SlotState, numSlots> slots;
};

//==============================================================================
namespace models
{
    struct Band { enum Type { highPass, lowPass, peak, lowShelf, highShelf } type; double hz, q, db; };
    struct Cabinet { const char* name; std::array<Band, 7> bands; };
    struct Mic     { const char* name; std::array<Band, 3> bands; };
    struct Room    { const char* name; float size, damping, wet; };

    inline constexpr int numCabinets = 5, numBassCabinets = 5, numMics = 5, numRooms = 5;

    inline const std::array<Cabinet, numCabinets>& cabinets()
    {
        using B = Band;
        static const std::array<Cabinet, numCabinets> list { {
            { "JSG 4x12 Modern",    { { { B::highPass, 80, 0.9, 0 },  { B::peak, 125, 1.1, 3.0 }, { B::peak, 420, 1.0, -3.5 }, { B::peak, 2500, 1.3, 5.0 }, { B::peak, 3700, 2.2, 2.5 }, { B::lowPass, 4600, 0.60, 0 }, { B::lowPass, 5100, 0.90, 0 } } } },
            { "JSG 4x12 Vintage",   { { { B::highPass, 85, 0.8, 0 },  { B::peak, 110, 1.0, 2.5 }, { B::peak, 500, 0.9, -2.0 }, { B::peak, 2100, 1.2, 3.5 }, { B::peak, 3300, 2.0, 1.5 }, { B::lowPass, 4200, 0.60, 0 }, { B::lowPass, 4700, 0.80, 0 } } } },
            { "JSG 2x12 Alnico",    { { { B::highPass, 95, 0.8, 0 },  { B::peak, 140, 1.0, 2.0 }, { B::peak, 350, 1.0, -1.5 }, { B::peak, 2800, 1.1, 4.0 }, { B::peak, 4300, 1.8, 3.0 }, { B::lowPass, 5600, 0.60, 0 }, { B::lowPass, 6200, 0.90, 0 } } } },
            { "JSG 1x12 Open Back", { { { B::highPass, 110, 0.7, 0 }, { B::peak, 160, 1.0, 1.5 }, { B::peak, 300, 0.8, -2.5 }, { B::peak, 2300, 1.1, 3.0 }, { B::peak, 3900, 1.8, 2.0 }, { B::lowPass, 5200, 0.60, 0 }, { B::lowPass, 5800, 0.80, 0 } } } },
            { "JSG 4x10 Blues",     { { { B::highPass, 100, 0.8, 0 }, { B::peak, 180, 1.0, 2.0 }, { B::peak, 600, 1.0, -2.0 }, { B::peak, 1800, 1.0, 4.0 }, { B::peak, 3200, 1.8, 2.0 }, { B::lowPass, 3900, 0.60, 0 }, { B::lowPass, 4500, 0.80, 0 } } } },
        } };
        return list;
    }

    inline const std::array<Cabinet, numBassCabinets>& bassCabinets()
    {
        using B = Band;
        static const std::array<Cabinet, numBassCabinets> list { {
            { "JSG 8x10",            { { { B::highPass, 35, 0.7, 0 },  { B::peak, 80, 1.0, 3.0 },  { B::peak, 400, 1.0, -2.0 }, { B::peak, 1500, 1.2, 2.0 }, { B::peak, 3000, 1.5, -1.0 }, { B::lowPass, 4000, 0.60, 0 }, { B::lowPass, 4800, 0.80, 0 } } } },
            { "JSG 4x10 Basse",      { { { B::highPass, 40, 0.7, 0 },  { B::peak, 100, 1.0, 2.5 }, { B::peak, 500, 1.0, -1.5 }, { B::peak, 2000, 1.2, 3.0 }, { B::peak, 3500, 1.6, 1.0 },  { B::lowPass, 5000, 0.60, 0 }, { B::lowPass, 6000, 0.80, 0 } } } },
            { "JSG 4x10 + tweeter",  { { { B::highPass, 40, 0.7, 0 },  { B::peak, 90, 1.0, 2.5 },  { B::peak, 450, 1.0, -2.0 }, { B::peak, 2000, 1.2, 2.0 }, { B::peak, 6000, 1.0, 3.0 },  { B::lowPass, 9000, 0.60, 0 }, { B::lowPass, 11000, 0.80, 0 } } } },
            { "JSG 1x15",            { { { B::highPass, 30, 0.7, 0 },  { B::peak, 70, 0.9, 4.0 },  { B::peak, 300, 1.0, -2.0 }, { B::peak, 1200, 1.0, 1.0 }, { B::peak, 2500, 1.5, -2.0 }, { B::lowPass, 3000, 0.60, 0 }, { B::lowPass, 3600, 0.80, 0 } } } },
            { "JSG 2x12 Basse",      { { { B::highPass, 45, 0.7, 0 },  { B::peak, 120, 1.0, 2.0 }, { B::peak, 600, 1.0, -1.0 }, { B::peak, 2200, 1.2, 3.0 }, { B::peak, 4000, 1.6, 1.5 },  { B::lowPass, 5500, 0.60, 0 }, { B::lowPass, 6500, 0.80, 0 } } } },
        } };
        return list;
    }

    inline const std::array<Mic, numMics>& mics()
    {
        using B = Band;
        static const std::array<Mic, numMics> list { {
            { "Dynamique",   { { { B::peak, 5000, 1.0, 3.0 },      { B::peak, 300, 0.9, -1.0 }, { B::highPass, 90, 0.7, 0 } } } },
            { "Ruban",       { { { B::highShelf, 4500, 0.7, -4.0 }, { B::peak, 180, 0.7, 2.0 },  { B::peak, 1200, 0.8, -0.5 } } } },
            { "Dyn. + Ruban",{ { { B::peak, 4800, 1.0, 1.5 },      { B::peak, 180, 0.7, 1.0 },  { B::highShelf, 6000, 0.7, -0.5 } } } },
            { "Dyn. large",  { { { B::peak, 3500, 0.9, 2.5 },      { B::peak, 110, 0.9, 1.5 },  { B::peak, 800, 0.8, -2.0 } } } },
            { "Statique",    { { { B::highShelf, 8000, 0.7, 1.5 },  { B::peak, 120, 0.8, 1.0 },  { B::peak, 2000, 0.7, -0.5 } } } },
        } };
        return list;
    }

    inline const std::array<Mic, numMics>& bassMics()
    {
        using B = Band;
        static const std::array<Mic, numMics> list { {
            { "Dynamique",   { { { B::peak, 4000, 1.0, 2.5 },      { B::peak, 300, 0.9, -1.0 }, { B::highPass, 28, 0.7, 0 } } } },
            { "Ruban",       { { { B::highShelf, 4500, 0.7, -4.0 }, { B::peak, 100, 0.7, 2.0 },  { B::peak, 1200, 0.8, -0.5 } } } },
            { "Dyn. + Ruban",{ { { B::peak, 4000, 1.0, 1.0 },      { B::peak, 100, 0.7, 1.0 },  { B::highShelf, 6000, 0.7, -0.5 } } } },
            { "Dyn. large",  { { { B::peak, 3000, 0.9, 2.0 },      { B::peak, 70, 0.9, 2.0 },   { B::peak, 700, 0.8, -2.0 } } } },
            { "Statique",    { { { B::highShelf, 8000, 0.7, 1.5 },  { B::peak, 80, 0.8, 1.0 },   { B::peak, 2000, 0.7, -0.5 } } } },
        } };
        return list;
    }

    inline const std::array<Room, numRooms>& rooms()
    {
        static const std::array<Room, numRooms> list { {
            { "Studio A",     0.22f, 0.65f, 0.14f },
            { "Studio B",     0.38f, 0.50f, 0.18f },
            { "Live Room",    0.55f, 0.35f, 0.24f },
            { "Grande salle", 0.80f, 0.40f, 0.26f },
            { "Cave",         0.68f, 0.85f, 0.28f },
        } };
        return list;
    }

    inline void apply (Biquad& f, const Band& b, double fs)
    {
        switch (b.type)
        {
            case Band::highPass:  f.setHighPass  (fs, b.hz, b.q);        break;
            case Band::lowPass:   f.setLowPass   (fs, b.hz, b.q);        break;
            case Band::peak:      f.setPeak      (fs, b.hz, b.q, b.db);  break;
            case Band::lowShelf:  f.setLowShelf  (fs, b.hz, b.db);       break;
            case Band::highShelf: f.setHighShelf (fs, b.hz, b.db);       break;
        }
    }
}

//==============================================================================
class AmpEngine
{
public:
    static constexpr int   maxStages = 3;
    static constexpr float outputTrim = 0.5f;
    static constexpr double eqFrequencies[5] { 100.0, 250.0, 800.0, 2500.0, 6000.0 };

    AmpEngine()
    {
        using Voice = DrivePedal::Voice;
        rack[rackGate].pedal   = std::make_unique<NoiseGatePedal>();
        rack[rackComp].pedal   = std::make_unique<CompressorPedal>();
        rack[rackDrive + 0].pedal = std::make_unique<DrivePedal> (Voice::screamer);
        rack[rackDrive + 1].pedal = std::make_unique<DrivePedal> (Voice::overdrive);
        rack[rackDrive + 2].pedal = std::make_unique<DrivePedal> (Voice::distortion);
        rack[rackDrive + 3].pedal = std::make_unique<DrivePedal> (Voice::fuzz);
        rack[rackChorus].pedal = std::make_unique<ChorusPedal>();
        rack[rackDelay].pedal  = std::make_unique<DelayPedal>();
        rack[rackReverb].pedal = std::make_unique<ReverbPedal>();
    }

    void prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;
        osRate     = newSampleRate * 4.0;
        blockSize  = maxBlockSize;

        oversampler.prepare (maxBlockSize);
        roomReverb.setSampleRate (sampleRate);
        tunerBuffer.assign ((size_t) maxBlockSize, 0.0f);
        if (convolution != nullptr && convolution->getBlockSize() != maxBlockSize)
            dispose (std::move (convolution));   // IR à recharger pour la nouvelle taille de bloc

        for (auto* buffer : { &dry, &workLeft, &workRight, &slotDryLeft, &slotDryRight, &diBuffer })
            buffer->assign ((size_t) maxBlockSize, 0.0f);

        diDelay.assign ((size_t) diDelaySize, 0.0f);
        diLatency = jlimit (0, diDelaySize - 1, measureLatency());

        for (auto& slot : slots)
        {
            if (slot.hasPending) { dispose (std::move (slot.current)); slot.current = std::move (slot.pending); slot.currentType = slot.pendingType; slot.hasPending = false; }
            if (slot.current != nullptr) slot.current->prepare (sampleRate, maxBlockSize);
            slot.mix = 0.0f;
            slot.running = false;
        }

        for (auto& unit : rack)
        {
            unit.pedal->prepare (sampleRate, maxBlockSize);
            unit.mix = 0.0f;
            unit.running = false;
        }

        smoothCoef   = timeCoef (sampleRate, 0.010);
        osSmoothCoef = timeCoef (osRate, 0.020);
        switchStep   = 1.0f / (float) (sampleRate * 0.012);
        vuCoef       = timeCoef (sampleRate, 0.120);

        diHighPass.setHighPass (sampleRate, 25.0, 0.7071);
        cache = {};
        configureChannel();
        reset();
    }

    void reset()
    {
        oversampler.reset();
        if (convolution != nullptr) convolution->reset();
        roomReverb.reset();
        resetPreamp();

        for (auto& f : cabFilters) f.reset();
        for (auto& f : micFilters) f.reset();
        for (auto& f : eqFilters)  f.reset();
        presenceFilter.reset();  lowCut.reset();  highCut.reset();  diHighPass.reset();
        diPre.reset();  diCab.reset();  diMic.reset();
        for (auto& f : diStage) f.reset();
        std::fill (diDelay.begin(), diDelay.end(), 0.0f);
        diWrite = 0;
        diMix = -1.0f;

        for (auto& slot : slots)
        {
            if (slot.current != nullptr) slot.current->reset();
            slot.mix = 0.0f;
            slot.running = false;
        }

        for (auto& unit : rack)
        {
            unit.pedal->reset();
            unit.mix = 0.0f;
            unit.running = false;
        }

        inputGain = -1.0f;  outputGain = -1.0f;  userGain = -1.0f;
        stageGain = -1.0;   powerDrive = -1.0;
        switchGain = 1.0f;  powerMix = -1.0f;
        vuSquare[0] = vuSquare[1] = 0.0f;
        fresh = true;
    }

    int getLatencySamples() const noexcept { return diLatency; }

    //==========================================================================
    //==========================================================================
    // Objets retirés par le moteur (pédales, IR). Dans le navigateur, un seul fil fait tout :
    // ils sont détruits tout de suite. Sur PC, le fil audio ne doit ni allouer ni libérer de
    // mémoire : le programme donne ici un « ramasseur » qui les détruit dans un autre fil.
    struct Disposer
    {
        virtual ~Disposer() = default;
        virtual void dispose (std::unique_ptr<Pedal>) noexcept = 0;
        virtual void dispose (std::unique_ptr<Convolution>) noexcept = 0;
    };

    void setDisposer (Disposer* d) noexcept { disposer = d; }

    /** Prépare une IR pour ce moteur (allocations : hors du fil audio). Nul si l'IR est vide. */
    std::unique_ptr<Convolution> makeImpulse (const float* data, int length) const
    {
        auto c = std::make_unique<Convolution> (blockSize > 0 ? blockSize : 128);
        c->load (data, length);
        if (! c->isLoaded()) return nullptr;
        return c;
    }

    /** Remplace l'IR (fil audio : aucune allocation, l'ancienne part au ramasseur). */
    void setImpulse (std::unique_ptr<Convolution> c) noexcept
    {
        dispose (std::move (convolution));
        convolution = std::move (c);
    }

    /** Réponse impulsionnelle (déjà à la fréquence de travail) : préparation et remplacement d'un coup. */
    void loadImpulseResponse (const float* data, int length)   { setImpulse (makeImpulse (data, length)); }

    bool hasImpulseResponse() const noexcept { return convolution != nullptr && convolution->isLoaded(); }

    /** Fabrique une pédale du catalogue prête à jouer (allocations : hors du fil audio). */
    std::unique_ptr<Pedal> makePedal (int type) const
    {
        if (type < 0 || type >= (int) pedalTypes().size()) return nullptr;
        auto pedal = pedalTypes()[(size_t) type].create();
        pedal->prepare (sampleRate, blockSize > 0 ? blockSize : 128);
        return pedal;
    }

    /** Pose une pédale déjà fabriquée (nulle : vide l'emplacement). L'ancienne s'éteint en fondu. */
    void setSlotPedal (int index, std::unique_ptr<Pedal> pedal, int type) noexcept
    {
        if (index < 0 || index >= numSlots) return;
        auto& slot = slots[(size_t) index];
        dispose (std::move (slot.pending));   // une pédale en attente jamais utilisée
        slot.pending = std::move (pedal);
        slot.pendingType = slot.pending != nullptr ? type : -1;
        slot.hasPending = true;
    }

    /** Pose une pédale du catalogue (type, -1 : vide l'emplacement). */
    void setSlotPedal (int index, int type)   { setSlotPedal (index, makePedal (type), type); }

    //==========================================================================
    void process (float* mono, float* left, float* right, int n, const EngineParams& p) noexcept
    {
        if (n <= 0 || n > blockSize)
            return;

        std::copy_n (mono, n, dry.data());

        const float inTarget = dbToGain (p.inputDb);
        if (inputGain < 0.0f) inputGain = inTarget;

        float peakIn = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            inputGain += (inTarget - inputGain) * smoothCoef;
            mono[i] *= inputGain;
            peakIn = jmax (peakIn, std::abs (mono[i]));
        }
        inputPeak = jmax (inputPeak, peakIn);
        std::copy_n (mono, n, tunerBuffer.data());
        tunerCount = n;

        const float powerTarget = p.power ? 1.0f : 0.0f;
        if (powerMix < 0.0f) powerMix = powerTarget;

        if (! p.power && powerMix <= 0.0f)
        {
            writeOutput (dry.data(), dry.data(), left, right, n, p, 0.0f);
            return;
        }

        {
            float* l = workLeft.data();
            float* r = workRight.data();
            std::copy_n (mono, n, l);
            std::copy_n (mono, n, r);

            const bool rackActed  = processRackInput (l, r, n, p.rack);
            const bool slotsActed = processSlots (0, slotsPerRow, l, r, n, p);

            if (rackActed || slotsActed)
                for (int i = 0; i < n; ++i)
                    mono[i] = 0.5f * (l[i] + r[i]);
        }

        float switchTarget = 1.0f;

        if (fresh)
        {
            fresh = false;
            if (p.channel != channel || p.bassGuitar != bassMode)
            {
                channel = jlimit (0, 2, p.channel);
                bassMode = p.bassGuitar;
                configureChannel();
                resetPreamp();
                stageGain = -1.0;
            }
        }

        if (p.channel != channel || p.bassGuitar != bassMode)
        {
            if (switchGain <= 0.0f)
            {
                channel = jlimit (0, 2, p.channel);
                bassMode = p.bassGuitar;
                configureChannel();
                resetPreamp();
                stageGain = -1.0;
            }
            else
            {
                switchTarget = 0.0f;
            }
        }

        updateFilters (p);

        const float diTarget = bassMode ? jlimit (0.0f, 1.0f, p.diBlend) : 0.0f;
        if (diMix < 0.0f) diMix = diTarget;
        const bool useDi = diTarget > 0.0f || diMix > 0.0f;
        const bool viaCab = ! (p.irOn && hasImpulseResponse());

        for (int i = 0; i < n; ++i)
        {
            diDelay[(size_t) diWrite] = mono[i];
            int readAt = diWrite - diLatency;
            if (readAt < 0) readAt += diDelaySize;

            if (useDi)
            {
                double v = diHighPass.process ((double) diDelay[(size_t) readAt]);
                v = diPre.process (v);
                for (int st = 0; st < cfg.stages; ++st) v = diStage[(size_t) st].process (v);
                if (viaCab && p.cabOn) v = diCab.process (v);
                if (viaCab && p.micOn) v = diMic.process (v);
                diBuffer[(size_t) i] = (float) v;
            }
            else
            {
                diBuffer[(size_t) i] = 0.0f;
            }

            if (++diWrite >= diDelaySize) diWrite = 0;
        }

        // --- Préampli, tonalité et ampli de puissance, suréchantillonnés x4 ------------
        float* u = oversampler.up (mono, n);
        const int upN = n * 4;

        const double totalDb     = jmap ((double) jlimit (0.0f, 1.0f, p.gain), cfg.minDb, cfg.maxDb);
        const double stageTarget = std::pow (10.0, totalDb / (20.0 * (double) cfg.stages));
        if (stageGain < 0.0) stageGain = stageTarget;

        const double master = (double) jlimit (0.0f, 1.0f, p.master);
        const double powerTargetDrive = 3.0 * master * master;
        if (powerDrive < 0.0) powerDrive = powerTargetDrive;

        for (int j = 0; j < upN; ++j)
        {
            stageGain  += (stageTarget - stageGain) * (double) osSmoothCoef;
            powerDrive += (powerTargetDrive - powerDrive) * (double) osSmoothCoef;

            double v = (double) u[j];
            v = preHighPass.process (v);
            v = preMid.process (v);
            v = brightShelf.process (v);

            for (int s = 0; s < cfg.stages; ++s)
            {
                v = tube (v * stageGain, cfg.bias[(size_t) s]);
                v = dcBlock[(size_t) s].process (v);
                v = stageLowPass[(size_t) s].process (v);
                v = -v;
            }

            v *= cfg.level;
            v = toneBass.process (v);
            v = toneMid.process (v);
            v = toneTreble.process (v);

            v = std::tanh (v * powerDrive) * 0.9;

            u[j] = (float) v;
        }

        oversampler.down (mono, n);

        // --- Présence, baffle + micro (ou réponse impulsionnelle) ---------------------
        for (int i = 0; i < n; ++i)
            mono[i] = (float) presenceFilter.process ((double) mono[i]);

        if (p.irOn && hasImpulseResponse() && n == convolution->getBlockSize())
        {
            convolution->process (mono);
        }
        else
        {
            if (p.cabOn)
            {
                for (int i = 0; i < n; ++i)
                {
                    double v = (double) mono[i];
                    for (auto& f : cabFilters) v = f.process (v);
                    mono[i] = (float) (v * 0.8);
                }
            }

            if (p.micOn)
            {
                for (int i = 0; i < n; ++i)
                {
                    double v = (double) mono[i];
                    for (auto& f : micFilters) v = f.process (v);
                    mono[i] = (float) v;
                }
            }
        }

        if (bassMode)
            for (int i = 0; i < n; ++i) mono[i] *= bassMakeup;

        if (useDi)
        {
            const float sign = (cfg.stages % 2 == 1) ? -1.0f : 1.0f;
            for (int i = 0; i < n; ++i)
            {
                diMix += (diTarget - diMix) * smoothCoef;
                if (diMix < 1.0e-4f && diTarget <= 0.0f) diMix = 0.0f;
                mono[i] = mono[i] * (1.0f - diMix) + sign * diGain * bassMakeup * diBuffer[(size_t) i] * diMix;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            double v = highCut.process (lowCut.process ((double) mono[i]));

            if (p.eqOn)
                for (auto& f : eqFilters) v = f.process (v);

            if (switchGain < switchTarget)      switchGain = jmin (switchTarget, switchGain + switchStep);
            else if (switchGain > switchTarget) switchGain = jmax (switchTarget, switchGain - switchStep);

            mono[i] = (float) v * switchGain;
        }

        // --- Pédales de la rangée 2 : à partir d'ici le signal est stéréo ----------------
        float* l = workLeft.data();
        float* r = workRight.data();

        std::copy_n (mono, n, l);
        std::copy_n (mono, n, r);

        processSlots (slotsPerRow, numSlots, l, r, n, p);
        processRackEffects (l, r, n, p.rack);

        if (p.roomOn)
        {
            const auto& room = models::rooms()[(size_t) jlimit (0, models::numRooms - 1, p.roomType)];
            const float level = jlimit (0.0f, 1.0f, p.roomLevel) * 2.0f;

            Reverb::Parameters rp;
            rp.roomSize   = room.size;
            rp.damping    = room.damping;
            rp.width      = 1.0f;
            rp.wetLevel   = 0.5f * room.wet * level;
            rp.dryLevel   = 0.5f * (1.0f - 0.15f * room.wet * level);
            rp.freezeMode = 0.0f;
            roomReverb.setParameters (rp);
            roomReverb.processStereo (l, r, n);
            roomActive = true;
        }
        else if (roomActive)
        {
            roomReverb.reset();
            roomActive = false;
        }

        writeOutput (l, r, left, right, n, p, powerTarget);
    }

    //==========================================================================
    // Mesures lues par l'interface (remises à zéro à la lecture des crêtes)
    float inputPeak = 0.0f, outputPeak = 0.0f;
    float vuLevel[2] { 0.0f, 0.0f };
    std::vector<float> tunerBuffer;   // entrée du dernier bloc (après le gain d'entrée), pour l'accordeur
    int   tunerCount = 0;

    /** Vrai si l'emplacement contient une pédale dont le son est actif (pour les témoins). */
    bool isSlotRunning (int index) const noexcept { return slots[(size_t) index].running; }

private:
    struct ChannelConfig
    {
        int    stages;
        double minDb, maxDb;
        double level;
        double highPassHz;
        double midHz, midDb;
        double stageLowPassHz;
        std::array<double, maxStages> bias;
    };

    static ChannelConfig configFor (int ch, bool bass = false) noexcept
    {
        if (bass)
        {
            switch (ch)
            {
                case 0:  return { 1, -12.0, 14.0, 1.90,  20.0, 500.0, 0.0, 9000.0, { 0.05, 0.0, 0.0 } };
                case 2:  return { 3,  12.0, 48.0, 0.60,  40.0, 800.0, 4.0, 6000.0, { 0.20, 0.10, 0.25 } };
                case 1:
                default: return { 2,   0.0, 30.0, 0.70,  35.0, 700.0, 2.0, 7000.0, { 0.15, 0.20, 0.0 } };
            }
        }

        switch (ch)
        {
            case 0:  return { 1, -10.0, 18.0, 1.80,  55.0, 900.0, 0.0, 12000.0, { 0.10, 0.0, 0.0 } };
            case 2:  return { 3,  18.0, 60.0, 0.55, 170.0, 950.0, 6.0,  7500.0, { 0.25, 0.15, 0.30 } };
            case 1:
            default: return { 2,   4.0, 36.0, 0.65, 110.0, 800.0, 4.0,  9500.0, { 0.20, 0.30, 0.0 } };
        }
    }

    void configureChannel() noexcept
    {
        cfg = configFor (channel, bassMode);
        preHighPass.setHighPass (osRate, cfg.highPassHz, 0.7071);
        diPre.setHighPass (sampleRate, cfg.highPassHz, 0.7071);
        for (auto& f : diStage) f.setHighPass (sampleRate, 25.0, 0.7071);
        preMid.setPeak (osRate, cfg.midHz, 0.7, cfg.midDb);

        for (size_t s = 0; s < (size_t) maxStages; ++s)
        {
            dcBlock[s].setHighPass (osRate, 25.0, 0.7071);
            stageLowPass[s].setLowPass (osRate, cfg.stageLowPassHz, 0.7071);
        }
    }

    void resetPreamp() noexcept
    {
        preHighPass.reset();    preMid.reset();   brightShelf.reset();
        for (auto& f : dcBlock)      f.reset();
        for (auto& f : stageLowPass) f.reset();
        toneBass.reset();  toneMid.reset();  toneTreble.reset();
    }

    static inline double tube (double x, double bias) noexcept
    {
        return std::tanh (x + bias) - std::tanh (bias);
    }

    /** Retard du suréchantillonneur aux basses fréquences (pour aligner le DI de la basse). */
    int measureLatency()
    {
        Oversampler4 probe;
        probe.prepare (256);
        std::vector<float> in (256, 0.0f), out (256, 0.0f);
        // Réponse à une impulsion, puis centre de gravité de la réponse (retard de groupe en basse fréquence)
        in[0] = 1.0f;
        double weighted = 0.0, total = 0.0;
        for (int block = 0; block < 4; ++block)
        {
            float* u = probe.up (in.data(), 256);
            (void) u;
            probe.down (out.data(), 256);
            for (int i = 0; i < 256; ++i)
            {
                weighted += (double) out[(size_t) i] * (block * 256 + i);
                total += (double) out[(size_t) i];
            }
            in[0] = 0.0f;
        }
        return total != 0.0 ? (int) std::lround (weighted / total) : 0;
    }

    //==========================================================================
    struct Slot
    {
        std::unique_ptr<Pedal> current, pending;
        int currentType = -1, pendingType = -1;
        bool hasPending = false;
        float mix = 0.0f;
        bool  running = false;
    };

    bool processSlots (int first, int last, float* l, float* r, int n, const EngineParams& p) noexcept
    {
        bool any = false;

        for (int index = first; index < last; ++index)
        {
            auto& slot = slots[(size_t) index];
            const auto& state = p.slots[(size_t) index];
            bool changing = slot.hasPending;

            if (changing && (slot.mix <= 0.0f || slot.current == nullptr))
            {
                dispose (std::move (slot.current));
                slot.current = std::move (slot.pending);
                slot.currentType = slot.pendingType;
                slot.hasPending = false;
                slot.mix = 0.0f;
                slot.running = false;
                changing = false;
            }

            Pedal* pedal = slot.current.get();
            const float target = (pedal != nullptr && ! changing && state.on) ? 1.0f : 0.0f;

            if (pedal == nullptr || (target <= 0.0f && slot.mix <= 0.0f))
            {
                if (slot.running)
                {
                    if (pedal != nullptr && pedal->resetsWhenSwitchedOff()) pedal->reset();
                    slot.running = false;
                }
                continue;
            }

            float real[maxPedalKnobs];
            const auto& ranges = pedalTypes()[(size_t) slot.currentType].ranges;
            for (int k = 0; k < maxPedalKnobs; ++k)
                real[k] = ranges[(size_t) k].toReal (state.knobs[(size_t) k]);

            runPedal (*pedal, slot.mix, target, real, l, r, n);
            slot.running = true;
            any = true;
        }

        return any;
    }

    void runPedal (Pedal& pedal, float& mix, float target, const float* real, float* l, float* r, int n) noexcept
    {
        std::copy_n (l, n, slotDryLeft.data());
        std::copy_n (r, n, slotDryRight.data());

        if (mix < target)
        {
            float ramp = mix;
            for (int i = 0; i < n && ramp < 1.0f; ++i)
            {
                ramp = jmin (1.0f, ramp + switchStep);
                l[i] *= ramp;
                r[i] *= ramp;
            }
        }

        pedal.process (l, r, n, real);

        for (int i = 0; i < n; ++i)
        {
            if (mix < target)      mix = jmin (target, mix + switchStep);
            else if (mix > target) mix = jmax (target, mix - switchStep);

            const float wl = std::isfinite (l[i]) ? jlimit (-16.0f, 16.0f, l[i]) : 0.0f;
            const float wr = std::isfinite (r[i]) ? jlimit (-16.0f, 16.0f, r[i]) : 0.0f;

            const float dl = slotDryLeft[(size_t) i], dr = slotDryRight[(size_t) i];
            l[i] = dl + (wl - dl) * mix;
            r[i] = dr + (wr - dr) * mix;
        }
    }

    //==========================================================================
    enum { rackGate, rackComp, rackDrive, rackChorus = rackDrive + 4, rackDelay, rackReverb, numRackUnits };

    struct RackUnit
    {
        std::unique_ptr<Pedal> pedal;
        float mix = 0.0f;
        bool  running = false;
    };

    bool runRackUnit (int index, bool on, const float* real, float* l, float* r, int n) noexcept
    {
        auto& unit = rack[(size_t) index];
        const float target = on ? 1.0f : 0.0f;

        if (target <= 0.0f && unit.mix <= 0.0f)
        {
            if (unit.running)
            {
                unit.pedal->reset();
                unit.running = false;
            }
            return false;
        }

        runPedal (*unit.pedal, unit.mix, target, real, l, r, n);
        unit.running = true;
        return true;
    }

    bool processRackInput (float* l, float* r, int n, const RackState& s) noexcept
    {
        bool any = false;

        const float gate[maxPedalKnobs] = { s.gateThresholdDb, s.gateDecayMs, 0.0f, 0.0f };
        any |= runRackUnit (rackGate, s.gateOn, gate, l, r, n);

        const float comp[maxPedalKnobs] = { 5.0f, s.compAttackMs, s.compLevel, 0.0f };
        any |= runRackUnit (rackComp, s.compOn, comp, l, r, n);

        static constexpr float driveLevel[4] = { 7.0f, 4.0f, 2.0f, 2.0f };
        const int type = jlimit (0, 3, s.driveType);

        for (int t = 0; t < 4; ++t)
        {
            const float drive[maxPedalKnobs] = { s.drive, 5.0f, driveLevel[t], 0.0f };
            any |= runRackUnit (rackDrive + t, s.driveOn && t == type, drive, l, r, n);
        }

        return any;
    }

    void processRackEffects (float* l, float* r, int n, const RackState& s) noexcept
    {
        const float chorus[maxPedalKnobs] = { 0.8f, 5.0f, s.fxMix, 0.0f };
        runRackUnit (rackChorus, s.fxOn && s.chorusOn, chorus, l, r, n);

        const float delay[maxPedalKnobs] = { s.fxTimeMs, s.fxFeedback, s.fxLevel, 0.0f };
        runRackUnit (rackDelay, s.fxOn && s.delayOn, delay, l, r, n);

        const float reverb[maxPedalKnobs] = { s.fxLevel, 5.0f, 5.0f, 0.0f };
        runRackUnit (rackReverb, s.fxOn && s.reverbOn, reverb, l, r, n);
    }

    //==========================================================================
    struct Cache
    {
        std::array<float, 4> tone { -1.0f, -1.0f, -1.0f, -1.0f };
        std::array<float, 5> eq { -100.0f, -100.0f, -100.0f, -100.0f, -100.0f };
        float lowCut = -1.0f, highCut = -1.0f;
        int   cabType = -1, micType = -1, bright = -1, bass = -1;
    };

    static bool approach (float& current, float target, float unset, float tolerance) noexcept
    {
        if (current <= unset)                               { current = target; return true; }
        if (std::abs (target - current) <= tolerance)       { if (current != target) { current = target; return true; } return false; }
        current += (target - current) * 0.35f;
        return true;
    }

    void updateFilters (const EngineParams& p) noexcept
    {
        const auto knobDb = [] (float v, double range) { return ((double) jlimit (0.0f, 1.0f, v) - 0.5) * 2.0 * range; };

        bool toneChanged = cache.bass != (int) bassMode;
        cache.bass = (int) bassMode;
        const std::array<float, 4> tone { p.bass, p.mid, p.treble, p.presence };
        for (size_t i = 0; i < 4; ++i)
            toneChanged |= approach (cache.tone[i], tone[i], -0.5f, 1.0e-4f);

        if (toneChanged)
        {
            toneBass.setLowShelf    (osRate, bassMode ? 130.0 : 170.0, knobDb (cache.tone[0], 14.0));
            toneMid.setPeak         (osRate, bassMode ? 500.0 : 700.0, 0.8, knobDb (cache.tone[1], 10.0));
            toneTreble.setHighShelf (osRate, bassMode ? 2500.0 : 2800.0, knobDb (cache.tone[2], 12.0));
            presenceFilter.setPeak  (sampleRate, 4200.0, 0.9, knobDb (cache.tone[3], 8.0));
        }

        if (cache.bright != (int) p.bright)
        {
            cache.bright = (int) p.bright;
            if (p.bright) brightShelf.setHighShelf (osRate, 3200.0, 6.0);
            else          brightShelf.setBypass();
        }

        const int cabType = bassMode ? 100 + jlimit (0, models::numBassCabinets - 1, p.bassCabType)
                                     : jlimit (0, models::numCabinets - 1, p.cabType);
        if (cache.cabType != cabType)
        {
            cache.cabType = cabType;
            const auto& bands = bassMode ? models::bassCabinets()[(size_t) (cabType - 100)].bands
                                         : models::cabinets()[(size_t) cabType].bands;
            for (size_t i = 0; i < bands.size(); ++i) models::apply (cabFilters[i], bands[i], sampleRate);
            diFollow (diCab, bands.data(), bands.size());
        }

        const int micIndex = jlimit (0, models::numMics - 1, p.micType);
        const int micType = micIndex + (bassMode ? 100 : 0);
        if (cache.micType != micType)
        {
            cache.micType = micType;
            const auto& bands = (bassMode ? models::bassMics() : models::mics())[(size_t) micIndex].bands;
            for (size_t i = 0; i < bands.size(); ++i) models::apply (micFilters[i], bands[i], sampleRate);
            diFollow (diMic, bands.data(), bands.size());
        }

        if (approach (cache.lowCut, p.lowCutHz, 0.0f, 0.05f))    lowCut.setHighPass (sampleRate, (double) cache.lowCut, 0.7071);
        if (approach (cache.highCut, p.highCutHz, 0.0f, 1.0f))   highCut.setLowPass (sampleRate, (double) cache.highCut, 0.7071);

        for (size_t i = 0; i < 5; ++i)
            if (approach (cache.eq[i], p.eqDb[i], -50.0f, 0.01f))
                eqFilters[i].setPeak (sampleRate, eqFrequencies[i], 1.1, (double) cache.eq[i]);
    }

    void diFollow (Biquad& f, const models::Band* bands, size_t count) noexcept
    {
        for (size_t i = 0; i < count; ++i)
            if (bands[i].type == models::Band::highPass) { models::apply (f, bands[i], sampleRate); return; }
        f.setBypass();
    }

    //==========================================================================
    void writeOutput (const float* wetL, const float* wetR, float* left, float* right, int n,
                      const EngineParams& p, float powerTarget) noexcept
    {
        const float user = p.mute ? 0.0f : dbToGain (p.outputDb);
        const float outTarget = outputTrim * user;
        if (outputGain < 0.0f) outputGain = outTarget;
        if (userGain < 0.0f)   userGain = user;

        float peak = 0.0f;
        float sqL = vuSquare[0], sqR = vuSquare[1];

        for (int i = 0; i < n; ++i)
        {
            outputGain += (outTarget - outputGain) * smoothCoef;
            userGain   += (user - userGain) * smoothCoef;
            powerMix   += (powerTarget - powerMix) * smoothCoef;
            if (powerMix < 1.0e-4f && powerTarget <= 0.0f) powerMix = 0.0f;

            const float direct = dry[(size_t) i] * userGain * (1.0f - powerMix);
            const float outL = softLimit (wetL[i] * outputGain * powerMix + direct);
            const float outR = softLimit (wetR[i] * outputGain * powerMix + direct);

            left[i] = outL;
            right[i] = outR;

            peak = jmax (peak, std::abs (outL), std::abs (outR));
            sqL += (outL * outL - sqL) * vuCoef;
            sqR += (outR * outR - sqR) * vuCoef;
        }

        vuSquare[0] = undenormalise (sqL);
        vuSquare[1] = undenormalise (sqR);
        vuLevel[0] = std::sqrt (vuSquare[0]);
        vuLevel[1] = std::sqrt (vuSquare[1]);
        outputPeak = jmax (outputPeak, peak);
    }

    static inline float softLimit (float x) noexcept
    {
        const float a = std::abs (x);
        if (a <= 0.7f) return x;
        const float y = 0.7f + 0.3f * std::tanh ((a - 0.7f) / 0.3f);
        return x < 0.0f ? -y : y;
    }

    static float timeCoef (double fs, double seconds) noexcept
    {
        return 1.0f - std::exp (-1.0f / (float) (fs * seconds));
    }

    //==========================================================================
    double sampleRate = 48000.0, osRate = 192000.0;
    int blockSize = 0;

    Oversampler4 oversampler;
    std::unique_ptr<Convolution> convolution;
    Disposer* disposer = nullptr;

    template <typename T>
    void dispose (std::unique_ptr<T> object) noexcept
    {
        if (object != nullptr && disposer != nullptr) disposer->dispose (std::move (object));
        // sans ramasseur, l'objet est détruit ici
    }
    Reverb roomReverb;
    bool roomActive = false;

    int channel = 2;
    bool bassMode = false, fresh = true;
    ChannelConfig cfg = configFor (2);

    static constexpr int diDelaySize = 1024;
    static constexpr float diGain = 2.2f;
    static constexpr float bassMakeup = 1.8f;
    std::vector<float> diDelay, diBuffer;
    int diWrite = 0, diLatency = 0;
    float diMix = -1.0f;
    Biquad diHighPass, diPre, diCab, diMic;
    std::array<Biquad, maxStages> diStage;
    Cache cache;

    Biquad preHighPass, preMid, brightShelf;
    std::array<Biquad, maxStages> dcBlock, stageLowPass;
    Biquad toneBass, toneMid, toneTreble, presenceFilter;
    std::array<Biquad, 7> cabFilters;
    std::array<Biquad, 3> micFilters;
    std::array<Biquad, 5> eqFilters;
    Biquad lowCut, highCut;

    std::array<Slot, numSlots> slots;
    std::array<RackUnit, numRackUnits> rack;
    std::vector<float> dry, workLeft, workRight, slotDryLeft, slotDryRight;

    float  smoothCoef = 0.01f, osSmoothCoef = 0.01f, switchStep = 0.01f, vuCoef = 0.001f;
    float  inputGain = -1.0f, outputGain = -1.0f, userGain = -1.0f, switchGain = 1.0f, powerMix = -1.0f;
    double stageGain = -1.0, powerDrive = -1.0;
    float  vuSquare[2] { 0.0f, 0.0f };
};

} // namespace jsg
