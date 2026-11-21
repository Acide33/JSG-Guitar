# Bibliothèques et algorithmes tiers

Tout le code des dossiers `ui`, `sources/src`, `sources/moteur` et `sources/plugin` a été écrit pour JSG Guitar
(© Jérôme Soler, licence GPL v3 ou ultérieure). Il n'utilise ni ne contient de code de JUCE.

## Bibliothèques incluses

### Kit VST 3 de Steinberg (version 3.8) — plug-in VST3
Téléchargé à la compilation par `sources/plugin/build.bat` (non inclus dans ce dépôt), compilé dans
`JSG Guitar.vst3`. Licence MIT, compatible GPL.

    MIT License
    Copyright (c) 2026, Steinberg Media Technologies GmbH

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

VST est une marque déposée de Steinberg Media Technologies GmbH.

### dr_wav, dr_mp3, dr_flac — David Reid
Lecture des fichiers WAV, MP3 et FLAC. Dossier `sources/third_party`.
Licence au choix : domaine public (Unlicense) ou MIT-0. Compatible GPL.

### WebView2 — Microsoft
En-tête `WebView2.h` (compilation) et `WebView2Loader.dll` (à côté du programme et dans le plug-in, signé par Microsoft).
Dossier `sources/third_party/webview2`. Licence de type BSD : redistribution libre en gardant la mention
de copyright. Le moteur WebView2 lui-même fait partie de Windows.

    Copyright (C) Microsoft Corporation. All rights reserved.

Texte complet : [sources/third_party/webview2/LICENSE.txt](sources/third_party/webview2/LICENSE.txt)

## Algorithmes publics (réécrits pour JSG Guitar)

- **Réverbération** : algorithme Freeverb de Jezar at Dreampoint (2000), domaine public.
- **Suréchantillonnage** : filtres demi-bande IIR polyphasés selon la méthode de Laurent de Soras
  (bibliothèque « hiir », licence WTFPL).
- **Égaliseur** : formules publiques du « Audio EQ Cookbook » (R. Bristow-Johnson).
- **Accordeur** : méthode YIN (de Cheveigné et Kawahara, 2002).
- **Vitesse et tonalité du lecteur** : vocodeur de phase, porté depuis la version JUCE de JSG Guitar
  (code de Jérôme Soler) et réécrit sans JUCE.

## Fournis par macOS (non distribués)

- Son : Core Audio. Accès à l'entrée : AVFoundation.
- Encodage AAC : AudioToolbox.
- Fenêtre : WebKit (WKWebView), Cocoa.

## Fournis par Windows (non distribués)

- Son : API WASAPI.
- Encodage MP3 : Media Foundation.
- Fenêtre : WebView2 Runtime.

## Outils de compilation (non distribués)

Visual Studio / CMake, ou zig (MIT). Les programmes produits ne sont soumis à aucune condition de licence
de ces outils.
