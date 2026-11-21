// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Fenêtre du plug-in : l'interface de JSG Guitar (page web locale) affichée par WebView2 dans la
    fenêtre que donne le logiciel hôte. Windows seulement ; ailleurs, pas de fenêtre (réglages
    génériques du logiciel hôte).
*/
#include "pluginterfaces/gui/iplugview.h"

namespace jsg::plugin
{
class Plugin;
Steinberg::IPlugView* createEditorView (Plugin& plugin);
}
