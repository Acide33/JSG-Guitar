# SPDX-License-Identifier: GPL-3.0-or-later
# JSG Guitar — logiciel autonome (sans JUCE)
#
# Windows, Visual Studio 2022 (ou plus récent) :
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#   -> build/Release/JSG Guitar.exe, avec WebView2Loader.dll et le dossier « ui »
#
# Mac (Xcode ou outils en ligne de commande Xcode, macOS 12 ou plus récent) :
#   cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#   cmake --build build --config Release
#   -> build/Release/JSG Guitar.app (Mac Apple et Intel)
#
# Linux : version de test « jsg_test », avec une carte son simulée (fichiers WAV), sans fenêtre.
cmake_minimum_required (VERSION 3.20)

if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Version minimale de macOS")
endif ()

project (JSGGuitar VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

# Dossier de l'interface, copié à côté du programme (Windows) ou dans le paquet .app (Mac)
set (JSG_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui" CACHE PATH "Dossier de l'interface (index.html, js, css, img)")

# Partie commune : moteur, serveur local, lecteur, enregistreur, décodage
set (JSG_COMMON src/host.cpp src/server.cpp src/decode.cpp src/mp3_encoder.cpp)

if (EXISTS "${JSG_UI_DIR}/index.html")
    set (JSG_HAS_UI ON)
else ()
    message (WARNING "Interface introuvable dans ${JSG_UI_DIR} : copiez le dossier ui à côté du programme.")
endif ()

#==============================================================================
if (WIN32)
    enable_language (RC)
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # pas de redistribuable Visual C++ à installer

    add_executable (JSGGuitar WIN32 src/main_win.cpp src/wasapi.cpp src/webview_window.cpp ${JSG_COMMON} res/jsg.rc)
    set_target_properties (JSGGuitar PROPERTIES OUTPUT_NAME "JSG Guitar")
    target_include_directories (JSGGuitar PRIVATE src moteur third_party third_party/webview2)
    if (NOT MSVC)
        target_include_directories (JSGGuitar PRIVATE third_party/webview2/compat)   # EventToken.h pour MinGW
    endif ()
    target_compile_definitions (JSGGuitar PRIVATE UNICODE _UNICODE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_link_libraries (JSGGuitar PRIVATE ole32 avrt ws2_32 shell32 uuid user32 advapi32 gdi32 mfplat mfreadwrite)

    if (MSVC)
        target_compile_options (JSGGuitar PRIVATE /utf-8 /W3 /permissive- /Zc:__cplusplus $<$<CONFIG:Release>:/O2 /GL>)
        target_link_options (JSGGuitar PRIVATE /MANIFEST:NO $<$<CONFIG:Release>:/LTCG>)   # le manifeste est dans jsg.rc
    else ()
        target_compile_options (JSGGuitar PRIVATE -municode)
        target_link_options (JSGGuitar PRIVATE -municode)
    endif ()

    # Chargeur officiel de WebView2 (Microsoft, licence BSD), à côté du programme
    add_custom_command (TARGET JSGGuitar POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/WebView2Loader.dll" "$<TARGET_FILE_DIR:JSGGuitar>")

    if (JSG_HAS_UI)
        add_custom_command (TARGET JSGGuitar POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${JSG_UI_DIR}" "$<TARGET_FILE_DIR:JSGGuitar>/ui"
            COMMENT "Copie de l'interface")
    endif ()

#==============================================================================
elseif (APPLE)
    enable_language (OBJCXX)

    set (MACOSX_BUNDLE_EXECUTABLE_NAME "JSG Guitar")
    set (MACOSX_BUNDLE_GUI_IDENTIFIER "com.jeromesolergroup.jsgguitar")
    set (MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set (MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set (MACOSX_BUNDLE_COPYRIGHT "© 2026 Jérôme Soler — logiciel libre (GPL v3)")
    configure_file (res/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")

    set_source_files_properties (res/jsg.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable (JSGGuitar MACOSX_BUNDLE src/main_mac.mm src/coreaudio.mm ${JSG_COMMON} res/jsg.icns)
    set_target_properties (JSGGuitar PROPERTIES
        OUTPUT_NAME "JSG Guitar"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.jeromesolergroup.jsgguitar"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/res/jsg.entitlements"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")   # signature locale ; la signature Apple se fait à la publication
    target_include_directories (JSGGuitar PRIVATE src moteur third_party)
    target_compile_options (JSGGuitar PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -Wall -Wno-unused-parameter $<$<CONFIG:Release>:-O2>)
    target_link_libraries (JSGGuitar PRIVATE "-framework Cocoa" "-framework WebKit" "-framework CoreAudio"
                                             "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreFoundation")

    if (JSG_HAS_UI)
        # Interface dans JSG Guitar.app/Contents/Resources/ui (les playbacks JSG vont dans Musique/JSG Guitar/Playbacks)
        add_custom_command (TARGET JSGGuitar POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${JSG_UI_DIR}" "$<TARGET_BUNDLE_CONTENT_DIR:JSGGuitar>/Resources/ui"
            COMMENT "Copie de l'interface dans le paquet")
    endif ()

#==============================================================================
else ()
    # Linux : essais sans carte son (carte son « fichier », voir file_backend.cpp et main_test.cpp)
    find_package (Threads REQUIRED)
    add_executable (jsg_test src/main_test.cpp src/file_backend.cpp ${JSG_COMMON})
    target_include_directories (jsg_test PRIVATE src moteur third_party)
    target_compile_options (jsg_test PRIVATE -Wall -Wno-unused-parameter -O2)
    target_link_libraries (jsg_test PRIVATE Threads::Threads)
endif ()
