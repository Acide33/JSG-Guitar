#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# JSG Guitar pour Mac : compile l'application (Mac Apple et Intel) et fabrique « JSG Guitar.dmg ».
#
# Sur un Mac avec Xcode (ou les outils en ligne de commande Xcode) et CMake :
#     ./installateur/creer_dmg_mac.sh
# Résultat : installateur/sortie/JSG Guitar.dmg
#
# Signature Apple (facultative, compte Apple Developer) :
#     export JSG_MAC_IDENTITY="Developer ID Application: Votre Nom (EQUIPE)"
#   puis, pour la notarisation, au choix :
#     export JSG_NOTARY_PROFILE="profil"   (créé une fois avec : xcrun notarytool store-credentials)
#   ou  export APPLE_ID=... APPLE_TEAM_ID=... APPLE_APP_PASSWORD=...   (mot de passe d'application)
# Sans signature Apple, l'application est signée localement : macOS demandera une autorisation
# au premier lancement (voir LISEZMOI-MAC.txt).
set -euo pipefail

cd "$(dirname "$0")/.."
RACINE="$(pwd)"
BUILD="$RACINE/sources/build-mac"
SORTIE="$RACINE/installateur/sortie"
ENTITLEMENTS="$RACINE/sources/res/jsg.entitlements"

echo "=== 1/3  Compilation (arm64 + x86_64) ==="
cmake -S "$RACINE/sources" -B "$BUILD" -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build "$BUILD" --config Release
APP="$BUILD/Release/JSG Guitar.app"
[ -d "$APP" ] || { echo "Application introuvable : $APP"; exit 1; }
lipo -info "$APP/Contents/MacOS/JSG Guitar"

echo "=== 2/3  Signature ==="
if [ -n "${JSG_MAC_IDENTITY:-}" ]; then
    codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$JSG_MAC_IDENTITY" "$APP"
else
    echo "Pas de JSG_MAC_IDENTITY : signature locale (ad hoc)."
    codesign --force --options runtime --entitlements "$ENTITLEMENTS" --sign - "$APP"
fi
codesign --verify --strict --verbose=2 "$APP"

echo "=== 3/3  Image disque ==="
STAGE="$(mktemp -d)/JSG Guitar"
mkdir -p "$STAGE" "$SORTIE"
ditto "$APP" "$STAGE/JSG Guitar.app"
ln -s /Applications "$STAGE/Applications"
cp "$RACINE/installateur/LISEZMOI-MAC.txt" "$STAGE/LISEZMOI.txt"
cp "$RACINE/LICENSE" "$STAGE/LICENCE-GPLv3.txt"
cp "$RACINE/LICENCE-CONTENUS.md" "$STAGE/LICENCE-CONTENUS.txt"
cp "$RACINE/LICENCES-TIERS.md" "$STAGE/LICENCES-TIERS.txt"

DMG="$SORTIE/JSG Guitar.dmg"
rm -f "$DMG"
hdiutil create -volname "JSG Guitar" -srcfolder "$STAGE" -ov -format UDZO "$DMG"

if [ -n "${JSG_MAC_IDENTITY:-}" ]; then
    codesign --force --timestamp --sign "$JSG_MAC_IDENTITY" "$DMG"
    if [ -n "${JSG_NOTARY_PROFILE:-}" ]; then
        xcrun notarytool submit "$DMG" --keychain-profile "$JSG_NOTARY_PROFILE" --wait
    elif [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
        xcrun notarytool submit "$DMG" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait
    else
        echo "Pas d'identifiants de notarisation : image signée mais pas notarisée."
        exit 0
    fi
    xcrun stapler staple "$DMG"
fi

echo
echo "Terminé : $DMG"
