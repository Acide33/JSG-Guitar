// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Usine du plug-in : ce que le logiciel hôte voit en scannant JSG Guitar.vst3.
*/
#include "plugin.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// Identifiant unique de JSG Guitar (ne jamais le changer : les projets enregistrés s'y réfèrent)
static const FUID jsgGuitarUid (0x5AF724B0, 0xDAFD4B38, 0x963AD256, 0xB5E891B6);

BEGIN_FACTORY_DEF (JSG_VENDOR, JSG_URL, JSG_EMAIL)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (jsgGuitarUid),
                PClassInfo::kManyInstances,
                kVstAudioEffectClass,
                "JSG Guitar",
                0,                                  // un seul composant (traitement + réglages)
                "Fx|Guitar|Distortion",
                JSG_VERSION_STR,
                kVstVersionString,
                jsg::plugin::Plugin::createInstance)

END_FACTORY
