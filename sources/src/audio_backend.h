// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Accès à la carte son, indépendant du système : WASAPI sous Windows (wasapi.cpp),
    fichier de test sous Linux (pour les essais sans carte son).
*/

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jsg
{

struct AudioDevice
{
    std::string id, name;
    bool isDefault = false;
};

struct AudioConfig
{
    std::string inputId, outputId;     // vides : appareils par défaut ; inputId "none" : pas d'entrée
    bool exclusive = true;             // WASAPI exclusif (faible latence) ; sinon partagé
    int bufferFrames = 128;            // taille de bloc voulue (exclusif)
    int sampleRate = 48000;            // fréquence voulue (exclusif ; en partagé : celle de Windows)
};

struct AudioInfo
{
    bool running = false, exclusive = false;
    int sampleRate = 0, blockSize = 0, inputChannels = 0, outputChannels = 0;
    double inputLatencyMs = 0.0, outputLatencyMs = 0.0;
    std::string inputName, outputName, format, note;
};

class AudioBackend
{
public:
    /** Appelé dans le fil audio : `frames` vaut toujours AudioInfo::blockSize.
        input[c] : canaux d'entrée (inputChannels), output[c] : canaux de sortie à remplir. */
    using Callback = std::function<void (const float* const* input, int inputChannels, float* const* output, int outputChannels, int frames)>;
    /** Appelé (dans le fil audio) juste avant le premier bloc, avec la fréquence et la taille de bloc. */
    using Prepare = std::function<void (int sampleRate, int blockSize)>;

    virtual ~AudioBackend() = default;
    virtual std::vector<AudioDevice> inputs() = 0;
    virtual std::vector<AudioDevice> outputs() = 0;

    /** Démarre le son. Faux, avec la raison dans `error`, si impossible. */
    virtual bool start (const AudioConfig& config, Prepare prepare, Callback callback, AudioInfo& info, std::string& error) = 0;
    virtual void stop() = 0;

    /** Libère la carte son pour les autres logiciels (mode exclusif) sans quitter. */
    virtual bool isRunning() const = 0;

    /** Entrée en attente dans la file (ms) et nombre de fois où l'entrée n'a pas suivi : pour le diagnostic. */
    virtual double inputQueueMs() const { return 0.0; }
    virtual int glitchCount() const { return 0; }
};

std::unique_ptr<AudioBackend> createAudioBackend();

} // namespace jsg
