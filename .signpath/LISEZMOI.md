# Signature SignPath — réglages une fois le projet accepté

La compilation GitHub signe les fichiers Windows d'une Release (étiquette `v…`) quand ces réglages existent.
Sans eux, elle continue de produire des fichiers non signés.

## 1. Dans SignPath (app.signpath.io)

- Projet : `JSG-Guitar`, relié au dépôt GitHub `Acide33/JSG-Guitar` (« trusted build system » GitHub).
- Politique de signature : `release-signing`, avec **approbation manuelle** par @Acide33.
- Deux configurations d'artefact, à copier depuis ce dossier :
  - `binaires` : [artifact-configurations/binaires.xml](artifact-configurations/binaires.xml) — `JSG Guitar.exe` et le plug-in ;
  - `installateur` : [artifact-configurations/installateur.xml](artifact-configurations/installateur.xml) — `JSG Guitar Setup.exe`.
- Un utilisateur « CI » avec un jeton d'API (API token), autorisé à soumettre des demandes sur le projet.

À chaque nouvelle version, mettez `product-version` à jour dans les deux fichiers XML (et dans SignPath),
en même temps que `sources/res/jsg.rc`, `sources/plugin/res/plugin.rc`, `sources/plugin/src/version.h`,
`sources/CMakeLists.txt` et `installateur/JSGGuitar.iss`.

## 2. Dans GitHub (Settings > Secrets and variables > Actions)

| Type | Nom | Valeur |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | le jeton d'API de l'utilisateur CI |
| Variable | `SIGNPATH_ORGANIZATION_ID` | l'identifiant d'organisation SignPath |
| Variable (facultative) | `SIGNPATH_PROJECT_SLUG` | si différent de `JSG-Guitar` |
| Variable (facultative) | `SIGNPATH_SIGNING_POLICY_SLUG` | si différent de `release-signing` |

## 3. Publier une version signée

Créez l'étiquette (ex. `v0.2.0`). La compilation s'arrête sur l'étape « Signature… » : approuvez la demande
sur SignPath (deux demandes : logiciel + plug-in, puis installateur). La Release est publiée ensuite.
