// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Fonctions exportées vers JavaScript (WebAssembly). Le processeur audio du navigateur
    (worklet.js) les appelle ; l'interface ne parle qu'au worklet.

    Les réglages sont rangés dans le même ordre que dans Source/PluginProcessor.cpp
    (énumération Param), puis, pour chaque emplacement du pédalier : marche/arrêt et 4 potards.
    Les valeurs sont les valeurs réelles (dB, Hz, 0..10, numéro de choix), comme dans le plug-in ;
    les potards des pédales sont des positions 0..1.
*/

#include "engine.h"

namespace
{
    enum Param
    {
        pPower, pMute,
        pChannel, pBright, pInput, pGain, pBass, pMid, pTreble, pPresence, pMaster,
        pCabOn, pCabType, pMicOn, pMicType, pRoomOn, pRoomType, pRoomLevel, pLowCut, pHighCut, pIrOn,
        pEqOn, pEq1, pEq2, pEq3, pEq4, pEq5,
        pOutput,
        pGateOn, pGateThreshold, pGateDecay,
        pCompOn, pCompLevel, pCompAttack,
        pDriveOn, pDriveType, pDrive,
        pFxOn, pFxChorus, pFxDelay, pFxReverb, pFxMix, pFxTime, pFxFeedback, pFxLevel,
        pInstrument, pBassCab, pDiBlend, pBassStrings,
        numFixedParams
    };

    constexpr int paramsPerSlot = 1 + jsg::maxPedalKnobs;
    constexpr int numParams = numFixedParams + jsg::numSlots * paramsPerSlot;
    constexpr int maxBlock = 1024;

    jsg::AmpEngine* engine = nullptr;
    float values[numParams] {};
    float inBuffer[maxBlock] {}, outLeft[maxBlock] {}, outRight[maxBlock] {};
    std::vector<float> impulse;

    float value (int i) { return values[i]; }
    bool  on (int i)    { return values[i] > 0.5f; }
    int   index (int i) { return (int) std::lround (values[i]); }
    float knob (int i)  { return values[i] * 0.1f; }

    void makeParams (jsg::EngineParams& p)
    {
        p.power = on (pPower);              p.mute = on (pMute);
        p.channel = index (pChannel);       p.bright = on (pBright);          p.inputDb = value (pInput);
        p.gain = knob (pGain);              p.bass = knob (pBass);            p.mid = knob (pMid);
        p.treble = knob (pTreble);          p.presence = knob (pPresence);    p.master = knob (pMaster);

        p.bassGuitar = index (pInstrument) == 1;
        p.bassCabType = index (pBassCab);   p.diBlend = knob (pDiBlend);
        p.cabOn = on (pCabOn);              p.cabType = index (pCabType);
        p.micOn = on (pMicOn);              p.micType = index (pMicType);
        p.roomOn = on (pRoomOn);            p.roomType = index (pRoomType);   p.roomLevel = knob (pRoomLevel);
        p.lowCutHz = value (pLowCut);       p.highCutHz = value (pHighCut);   p.irOn = on (pIrOn);

        p.eqOn = on (pEqOn);
        for (int b = 0; b < 5; ++b) p.eqDb[(size_t) b] = value (pEq1 + b);

        p.outputDb = value (pOutput);

        auto& rack = p.rack;
        rack.gateOn = on (pGateOn);         rack.gateThresholdDb = value (pGateThreshold);   rack.gateDecayMs = value (pGateDecay);
        rack.compOn = on (pCompOn);         rack.compLevel = value (pCompLevel);             rack.compAttackMs = value (pCompAttack);
        rack.driveOn = on (pDriveOn);       rack.driveType = index (pDriveType);             rack.drive = value (pDrive);
        rack.fxOn = on (pFxOn);             rack.chorusOn = on (pFxChorus);
        rack.delayOn = on (pFxDelay);       rack.reverbOn = on (pFxReverb);
        rack.fxMix = value (pFxMix);        rack.fxTimeMs = value (pFxTime);
        rack.fxFeedback = value (pFxFeedback);   rack.fxLevel = value (pFxLevel);

        for (int slot = 0; slot < jsg::numSlots; ++slot)
        {
            auto& s = p.slots[(size_t) slot];
            const int base = numFixedParams + slot * paramsPerSlot;
            s.on = values[base] > 0.5f;
            for (int k = 0; k < jsg::maxPedalKnobs; ++k)
                s.knobs[(size_t) k] = values[base + 1 + k];
        }
    }

    jsg::EngineParams params;
}

#define EXPORT extern "C" __attribute__((visibility ("default")))

EXPORT int jsg_num_params()  { return numParams; }

EXPORT void jsg_init (float sampleRate)
{
    if (engine == nullptr) engine = new jsg::AmpEngine();
    engine->prepare ((double) sampleRate, 128);
}

EXPORT void jsg_set (int i, float v)                 { if (i >= 0 && i < numParams) values[i] = v; }
EXPORT void jsg_set_slot_pedal (int slot, int type)  { if (engine != nullptr) engine->setSlotPedal (slot, type); }

EXPORT float* jsg_input()  { return inBuffer; }
EXPORT float* jsg_left()   { return outLeft; }
EXPORT float* jsg_right()  { return outRight; }

/** Traite n échantillons (multiple de 128) : jsg_input() -> jsg_left() / jsg_right(). */
EXPORT void jsg_process (int n)
{
    if (engine == nullptr) return;
    makeParams (params);

    for (int offset = 0; offset < n && offset + 128 <= maxBlock; offset += 128)
    {
        const int count = n - offset < 128 ? n - offset : 128;
        engine->process (inBuffer + offset, outLeft + offset, outRight + offset, count, params);
    }
}

// Mesures : crêtes depuis la dernière lecture, niveau moyen (RMS) des vu-mètres
EXPORT float jsg_input_peak()  { if (! engine) return 0; const float v = engine->inputPeak;  engine->inputPeak = 0;  return v; }
EXPORT float jsg_output_peak() { if (! engine) return 0; const float v = engine->outputPeak; engine->outputPeak = 0; return v; }
EXPORT float jsg_vu (int ch)   { return engine ? engine->vuLevel[ch & 1] : 0.0f; }
EXPORT float* jsg_tuner()      { return engine ? engine->tunerBuffer.data() : nullptr; }
EXPORT int   jsg_tuner_count() { return engine ? engine->tunerCount : 0; }
EXPORT int   jsg_latency()     { return engine ? engine->getLatencySamples() : 0; }

// Réponse impulsionnelle : réserver, remplir depuis JavaScript, valider
EXPORT float* jsg_ir_buffer (int length)   { impulse.assign ((size_t) (length > 0 ? length : 1), 0.0f); return impulse.data(); }
EXPORT void   jsg_ir_commit (int length)
{
    if (engine != nullptr && length > 0 && length <= (int) impulse.size())
        engine->loadImpulseResponse (impulse.data(), length);
}
EXPORT int jsg_has_ir() { return engine && engine->hasImpulseResponse() ? 1 : 0; }
