# Code signing policy — Politique de signature

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

Signature gratuite fournie par [SignPath.io](https://about.signpath.io), certificat de la [SignPath Foundation](https://signpath.org)
(demande en cours : tant qu'elle n'est pas acceptée, les fichiers publiés ne sont pas signés).

## Ce qui est signé

Les fichiers Windows publiés dans les [Releases](https://github.com/Acide33/JSG-Guitar/releases) de ce dépôt :
- `JSG Guitar.exe` (le logiciel) ;
- `JSG Guitar.vst3` (le plug-in VST3) ;
- `JSG Guitar Setup.exe` (l'installateur).

Ils sont compilés par GitHub Actions, sur les machines de GitHub, à partir du code de ce dépôt
(fichier [.github/workflows/compilation.yml](.github/workflows/compilation.yml)), puis envoyés à SignPath,
qui vérifie leur origine avant de les signer. `WebView2Loader.dll` est fourni et signé par Microsoft.

## Équipe et rôles

| Rôle | Personne |
|---|---|
| Auteur (committer) | [Jérôme Soler (@Acide33)](https://github.com/Acide33) |
| Relecteur (reviewer) | [Jérôme Soler (@Acide33)](https://github.com/Acide33) — relit toute contribution extérieure avant intégration |
| Approbateur (approver) | [Jérôme Soler (@Acide33)](https://github.com/Acide33) — approuve à la main chaque demande de signature |

L'authentification à deux facteurs est activée sur les comptes GitHub et SignPath de l'équipe.

## Confidentialité — Privacy policy

This program will not transfer any information to other networked systems unless specifically requested
by the user or the person installing or operating it.

JSG Guitar ne transmet aucune information sur Internet : pas de compte, pas de statistiques, pas de
mise à jour automatique. En détail :
- le son, les réglages, les presets et les enregistrements restent sur l'ordinateur ;
- l'interface est servie par JSG Guitar lui-même, sur l'adresse locale 127.0.0.1, inaccessible depuis
  les autres ordinateurs ;
- un lien vers le site du groupe ou une page web ne s'ouvre que si vous cliquez dessus, dans votre navigateur ;
- l'interface s'affiche avec le moteur web de Windows (Microsoft Edge WebView2) ou de macOS (WebKit),
  soumis aux règles de confidentialité de Microsoft et d'Apple.

## Signaler un problème

Ouvrez une [Issue](https://github.com/Acide33/JSG-Guitar/issues) ou écrivez via [jeromesolergroup.com](https://jeromesolergroup.com).
