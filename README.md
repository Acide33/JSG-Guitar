# JSG Guitar

**Simulateur d'ampli guitare et basse, gratuit et libre, pour Windows et Mac — en logiciel autonome et en plug-in VST3 (Windows) — créé par le guitariste du Jérôme Soler Group.**

Branchez votre guitare dans votre carte son, mettez un casque, et jouez par-dessus les morceaux du groupe.

> *English below.*

---

## Le groupe derrière le logiciel

JSG Guitar est développé par **Jérôme Soler**, guitariste et chanteur du **Jérôme Soler Group (JSG)**, groupe pop rock de Gironde fondé en 2015.

Le groupe partage gratuitement **8 playbacks de JSG sans la guitare**, pour jouer avec lui :
Abrut shift, AT, Dissapoinement, Double tape, JS Confinement, Karamel, Rock F, Soso.

Ils forment un pack séparé, **« JSG Playbacks »**, sous licence **Creative Commons BY-NC 4.0** :
vous pouvez les partager, jouer dessus, les remixer et publier vos vidéos, en créditant le groupe,
tant que ce n'est pas commercial. Le pack fonctionne avec JSG Guitar mais aussi avec n'importe quel lecteur ou logiciel audio.

Discographie :
- ***STOP*** — 3ᵉ album (2026)
- ***Karamel Space Rabbit*** — 2ᵉ album (2024)
- ***AT*** — EP du Jérôme Soler Trio (2016)

🎸 Site du groupe : **[jeromesolergroup.com](http://jeromesolergroup.com)**
<!-- Ajoutez ici vos liens : Spotify, Deezer, YouTube, Bandcamp, Instagram… -->

Si JSG Guitar vous plaît, le meilleur moyen de nous remercier est d'**écouter et partager notre musique**.

---

## Fonctionnalités

- Simulation d'ampli **guitare** et **basse** (choix au démarrage), avec baffles, micros et pièces
- Pédales intégrées, égaliseur, réverbération, chargement de réponses impulsionnelles (IR)
- Accordeur
- Lecteur de playbacks avec réglage de la **vitesse** et de la **tonalité** (en demi-tons)
- Enregistrement **REC** en WAV 24 bits ou MP3 320 / 192 kbit/s
- Son en **WASAPI** avec mode exclusif pour une latence minimale, choix du tampon et de la fréquence
- Mesure de la **latence réelle** (avec un câble jack en boucle)
- **Plug-in VST3** pour votre logiciel musical (Reaper, Cubase, Studio One, FL Studio, Ableton Live, Bitwig…) : même son et même interface, 129 réglages automatisables, tout est enregistré dans le projet

Modes d'emploi : [GUIDE-UTILISATEUR.txt](GUIDE-UTILISATEUR.txt) (logiciel) et [GUIDE-PLUGIN.txt](GUIDE-PLUGIN.txt) (plug-in VST3)

## Télécharger

Les versions prêtes à l'emploi sont dans l'onglet **[Releases](../../releases)** du dépôt.
Téléchargez **`JSG Guitar Setup.exe`** et lancez-le : il installe le logiciel, le plug-in VST3
(dans `C:\Program Files\Common Files\VST3`) et, si vous le souhaitez, les playbacks JSG. Chaque partie peut être décochée.

**Playbacks JSG** : ils sont proposés dans l'installateur, ou en pack gratuit séparé sur [jeromesolergroup.com](http://jeromesolergroup.com) (copiez alors son dossier `playbacks` dans le dossier `ui` de JSG Guitar). Sans eux, le logiciel fonctionne normalement et vous pouvez jouer sur vos propres morceaux.

Configuration : Windows 10 ou 11 64 bits, une carte son avec entrée instrument. L'interface utilise WebView2, déjà présent sur Windows 10 et 11 à jour.

### Mac (prototype)

Téléchargez **`JSG Guitar.dmg`**, ouvrez-le et glissez JSG Guitar dans Applications.
macOS 12 ou plus récent, Mac Apple (M1 à M4) ou Intel. Au premier lancement, macOS demande l'accès au « micro » :
c'est l'entrée de votre carte son, acceptez. Si la version n'est pas signée par Apple, autorisez-la une fois dans
Réglages Système > Confidentialité et sécurité > « Ouvrir quand même ». Détails : [installateur/LISEZMOI-MAC.txt](installateur/LISEZMOI-MAC.txt).

Sur Mac, les prises compressées sont en AAC (.m4a) : macOS n'a pas d'encodeur MP3. Le plug-in VST3 / Audio Unit pour Mac est prévu.

## Compiler

Avec **Visual Studio 2022** (charge de travail « Développement Desktop en C++ »), **git** et, pour l'installateur, **[Inno Setup 6](http://jrsoftware.org/isinfo.php)** :

```bat
installateur\tout_compiler.bat
```

Ce script compile le logiciel et le plug-in, puis fabrique `installateur\sortie\JSG Guitar Setup.exe`.

Séparément :

```bat
sources\build.bat           :: logiciel  -> sources\build\Release\JSG Guitar.exe
sources\plugin\build.bat    :: plug-in   -> sources\plugin\build\VST3\Release\JSG Guitar.vst3
```

Le plug-in télécharge le kit VST3 de Steinberg (licence MIT) au premier passage.

**Sur Mac** (Xcode ou outils en ligne de commande Xcode, et CMake) :

```sh
./installateur/creer_dmg_mac.sh      # -> installateur/sortie/JSG Guitar.dmg
```

**Compilation automatique** : à chaque envoi sur GitHub, le fichier [.github/workflows/compilation.yml](.github/workflows/compilation.yml)
fait compiler la version Mac (`.dmg`) et la version Windows (installateur) par GitHub, gratuitement. Les fichiers sont dans
l'onglet **Actions**. Une étiquette `v0.1.0`, `v0.2.0`… les publie en plus dans une **Release**.

Ou à la main, pour le logiciel :

```bat
cd sources
cmake -B build -A x64
cmake --build build --config Release
```

Résultat : `sources\build\Release\JSG Guitar.exe`, avec le dossier `ui` et `WebView2Loader.dll` copiés à côté. Aucun redistribuable à installer.

Compilation croisée depuis Linux ou macOS avec zig 0.13 : `sources/build_zig.sh`.

### Organisation du code

| Dossier | Contenu |
|---|---|
| `sources/moteur` | Moteur audio C++ (ampli, pédales, DSP), commun au logiciel, au plug-in et à la version navigateur |
| `sources/src` | Programme : serveur local, lecteur, enregistreur, décodage (communs) ; Windows : WASAPI, fenêtre WebView2 ; Mac : Core Audio (`coreaudio.mm`), fenêtre WKWebView (`main_mac.mm`) |
| `sources/plugin` | Plug-in VST3 : paramètres, état du projet, fenêtre, tests |
| `sources/third_party` | dr_wav / dr_mp3 / dr_flac, WebView2 (Microsoft) |
| `installateur` | Installateur Windows (Inno Setup), image disque Mac (`creer_dmg_mac.sh`) |
| `.github/workflows` | Compilation automatique Mac / Windows / Linux par GitHub |
| `ui` | Interface (HTML / CSS / JavaScript), commune au logiciel et au plug-in |

## Licences

| Quoi | Licence |
|---|---|
| Code source (`sources/src`, `sources/moteur`, `ui/js`, `ui/css`, `ui/index.html`, scripts de compilation) | **GPL v3 ou ultérieure** — voir [LICENSE](LICENSE) |
| Images de l'interface (`ui/img`) | **CC BY-SA 4.0** — voir [LICENCE-CONTENUS.md](LICENCE-CONTENUS.md) |
| Playbacks JSG (pack séparé, non inclus dans ce dépôt) | **CC BY-NC 4.0** — voir la licence fournie avec le pack |
| Nom « JSG Guitar », « JSG Bass », « Jérôme Soler Group » et logos | Marques réservées : une version modifiée doit changer de nom — voir [LICENCE-CONTENUS.md](LICENCE-CONTENUS.md) |
| Bibliothèques incluses | Voir [LICENCES-TIERS.md](LICENCES-TIERS.md) |

En bref : vous pouvez utiliser, étudier, modifier et redistribuer le **code** (GPL v3) et les **images** (CC BY-SA 4.0), à condition que vos versions restent libres et changent de nom. Les **playbacks** sont partagés à part, en CC BY-NC 4.0.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org) (demande en cours).
Rôles de l'équipe, fichiers signés et politique de confidentialité : **[CODE_SIGNING.md](CODE_SIGNING.md)**.

**Confidentialité** : JSG Guitar ne transmet aucune information sur Internet (pas de compte, pas de statistiques, pas de mise à jour automatique).

## Contribuer

Les corrections et idées sont les bienvenues : voir [CONTRIBUTING.md](CONTRIBUTING.md).

---

## English

**JSG Guitar** is a free and open-source guitar & bass amp simulator for Windows and macOS (standalone app; VST3 plug-in on Windows), made by Jérôme Soler, guitarist and singer of the French pop rock band **Jérôme Soler Group (JSG)**. The band also shares 8 JSG backing tracks without guitar as a separate free pack (**CC BY-NC 4.0**), so you can play along — download it from [jeromesolergroup.com](http://jeromesolergroup.com) and copy its `playbacks` folder into JSG Guitar's `ui` folder.

- Guitar and bass amp simulation, built-in pedals, EQ, reverb, IR loader, tuner
- Backing track player with tempo and pitch shifting, WAV / MP3 recording
- Low-latency WASAPI exclusive mode, real latency measurement

**Install:** download `JSG Guitar Setup.exe` (Windows) or `JSG Guitar.dmg` (macOS 12+, Apple Silicon and Intel) from the Releases page.

**Build:** Visual Studio 2022 + CMake + git (+ Inno Setup for the installer) — run `installateur\tout_compiler.bat`. macOS: Xcode + CMake — run `installateur/creer_dmg_mac.sh`. GitHub Actions builds both automatically.

**License:** source code GPL-3.0-or-later, interface artwork CC BY-SA 4.0. The names and logos "JSG Guitar" / "JSG" are reserved: forks must use another name — see `LICENCE-CONTENUS.md`. The backing-track pack is CC BY-NC 4.0.

**Code signing policy:** see [CODE_SIGNING.md](CODE_SIGNING.md). **Privacy:** JSG Guitar does not send any data over the Internet.

Like the software? Please listen to and share our music: **[jeromesolergroup.com](http://jeromesolergroup.com)**
