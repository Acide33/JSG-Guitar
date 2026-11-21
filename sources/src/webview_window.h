// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Fenêtre de JSG Guitar : l'interface affichée par WebView2, le moteur web de Windows
    (présent sur Windows 10 et 11). Pas d'extensions, pas de barre d'adresse, pas d'Edge.
*/
#include <windows.h>
#include <string>

namespace jsg
{

struct WindowOptions
{
    std::wstring url;               // adresse de l'interface (serveur local)
    std::wstring userDataFolder;    // profil WebView2 (presets, IR gardées par l'interface)
    std::wstring placementFile;     // position et taille de la fenêtre d'une fois sur l'autre
    std::wstring title = L"JSG Guitar";
    HICON icon = nullptr;
    bool devTools = false;          // outils de développement (F12, clic droit)
};

enum class WindowResult
{
    closed,         // l'utilisateur a fermé la fenêtre
    unavailable     // WebView2 absent ou en échec : utiliser une autre fenêtre (Edge)
};

/** Ouvre la fenêtre et tourne jusqu'à sa fermeture. À appeler depuis un fil STA. */
WindowResult runWebViewWindow (const WindowOptions& options, std::wstring& error);

/** Classe de la fenêtre (pour la retrouver si le programme est relancé). */
constexpr const wchar_t* windowClassName = L"JSGGuitarWindow";

} // namespace jsg
