// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Conversion d'une prise WAV en MP3, avec l'encodeur MP3 intégré à Windows (Media Foundation,
    Windows 8 et plus). Sur Mac : en AAC (.m4a), avec l'encodeur intégré à macOS (AudioToolbox).
    Aucune bibliothèque à fournir, aucune licence à gérer.
    Hors temps réel : appelé après l'enregistrement, dans un fil à part.
*/

#include <filesystem>
#include <string>

namespace jsg
{

/** Débits proposés (kbit/s). */
constexpr int mp3Bitrates[] = { 128, 192, 256, 320 };

/** Extension du fichier compressé : MP3 sous Windows ; sur Mac, AAC (.m4a), l'encodeur intégré à macOS
    (macOS n'a pas d'encodeur MP3). */
#ifdef __APPLE__
inline constexpr const char* compressedExtension = ".m4a";
#else
inline constexpr const char* compressedExtension = ".mp3";
#endif

/** Convertit wav en mp3 (stéréo, 44,1 ou 48 kHz). Faux avec la raison en cas d'échec. */
bool encodeMp3 (const std::filesystem::path& wav, const std::filesystem::path& mp3, int kbps, std::string& error);

} // namespace jsg
