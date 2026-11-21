// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    JSG Guitar en plug-in VST3 (kit VST3 de Steinberg, licence MIT ; aucun code JUCE).
    Un seul composant : traitement du son et réglages dans le même objet.
*/

#include "plugin_core.h"

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace jsg::plugin
{

class EditQueue;

class Plugin final : public Steinberg::Vst::SingleComponentEffect
{
public:
    Plugin();
    ~Plugin() override;

    static Steinberg::FUnknown* createInstance (void*) { return (Steinberg::Vst::IAudioProcessor*) new Plugin(); }

    // Composant et traitement
    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                      Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) override;
    Steinberg::uint32  PLUGIN_API getLatencySamples() override { return (Steinberg::uint32) core.latencySamples(); }
    Steinberg::uint32  PLUGIN_API getTailSamples() override { return Steinberg::Vst::kInfiniteTail; }   // réverbe, delay
    Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setProcessing (Steinberg::TBool) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;

    // Projet
    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) override;

    // Réglages côté « contrôleur » et fenêtre
    Steinberg::tresult PLUGIN_API setParamNormalized (Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) override;
    Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) override;

    Core& getCore() noexcept { return core; }

    /** Fil de l'interface du logiciel hôte : transmet les réglages touchés dans la fenêtre. */
    void flushUserEdits();

private:
    void syncParametersFromCore();

    Core core;

    // Réglages touchés dans l'interface (fil du serveur) -> logiciel hôte (fil de son interface)
    struct Edit { int index; float plain; };
    std::mutex editLock;
    std::vector<Edit> pendingEdits;
    bool pendingDirty = false;
    std::array<std::chrono::steady_clock::time_point, numParams> lastEdit {};
    std::array<bool, numParams> editing {};
    std::unique_ptr<EditQueue> queue;      // réveille le fil de l'interface (Windows : petite fenêtre cachée)
};

} // namespace jsg::plugin
