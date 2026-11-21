// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Lecture des fichiers audio de la version PC : WAV, MP3, FLAC (bibliothèques dr_wav, dr_mp3,
    dr_flac de David Reid — domaine public / MIT No Attribution).
    Le format est reconnu au contenu du fichier, pas à son extension.
*/

#include "player.h"

#include <string>
#include <vector>

namespace jsg
{

struct DecodedAudio
{
    int channels = 0;
    double sampleRate = 0.0;
    std::vector<float> interleaved;   // échantillons entrelacés
    size_t frames() const { return channels > 0 ? interleaved.size() / (size_t) channels : 0; }
};

/** Décode un fichier audio en mémoire. Faux (avec la raison) s'il n'est pas lisible. */
bool decodeAudio (const void* data, size_t size, DecodedAudio& out, std::string& error);

/** Fabrique un morceau stéréo (le mono est dupliqué, au-delà de 2 canaux on garde les 2 premiers). */
std::unique_ptr<Song> makeSong (const DecodedAudio& audio, const std::string& title, int overviewPoints);

/** Rééchantillonnage hors temps réel (sinus cardinal fenêtré) : pour les réponses impulsionnelles. */
std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate);

/** Canal gauche (ou unique) d'un fichier décodé. */
std::vector<float> firstChannel (const DecodedAudio& audio);

} // namespace jsg
