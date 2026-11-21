#!/bin/sh
# Compilation croisée de JSG Guitar.exe depuis Linux ou macOS avec zig 0.13 (vérification)
set -e
ZIG=${ZIG:-zig}
cd "$(dirname "$0")"
mkdir -p build_win
cd build_win
F="-target x86_64-windows-gnu -O2 -std=c++20 -municode -DUNICODE -D_UNICODE -DNDEBUG -I../src -I../moteur -I../third_party -I../third_party/webview2 -I../third_party/webview2/compat"
for f in host server wasapi decode mp3_encoder webview_window main_win; do "$ZIG" c++ $F -c ../src/$f.cpp -o $f.o & done
wait
"$ZIG" c++ -target x86_64-windows-gnu -municode host.o server.o wasapi.o decode.o mp3_encoder.o webview_window.o main_win.o ../res/jsg.rc -o "JSG Guitar.exe" \
  -lole32 -lavrt -lws2_32 -lshell32 -luuid -luser32 -ladvapi32 -lgdi32 -lmfplat -lmfreadwrite -Wl,--subsystem,windows -s
cp ../third_party/webview2/WebView2Loader.dll .
echo "build_win/JSG Guitar.exe"
