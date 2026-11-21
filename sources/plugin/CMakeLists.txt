# JSG Guitar — plug-in VST3 (kit VST3 de Steinberg, licence MIT ; aucun code JUCE)
#
# Windows, Visual Studio 2022 :
#   cmake -B build -A x64 -DVST3_SDK=chemin/vers/vst3sdk
#   cmake --build build --config Release
# Résultat : build/VST3/Release/JSG Guitar.vst3 (dossier à copier dans C:\Program Files\Common Files\VST3)
# Le kit VST3 se télécharge avec : git clone --recursive https://github.com/steinbergmedia/vst3sdk.git
# (build.bat le fait tout seul s'il manque)
cmake_minimum_required (VERSION 3.25)
project (JSGGuitarVST3 VERSION 0.1.0 LANGUAGES C CXX)

set (VST3_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk" CACHE PATH "Dossier du kit VST3 de Steinberg (github.com/steinbergmedia/vst3sdk)")
set (JSG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Sources communes avec JSG Guitar PC (moteur, src, third_party)")
set (JSG_UI "${CMAKE_CURRENT_SOURCE_DIR}/../../ui" CACHE PATH "Interface de JSG Guitar (index.html, js, css, img)")

set (SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set (SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set (SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "Exemples d'hôtes du kit (pour les tests)")
set (SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set (SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

if (MINGW AND NOT MSVC)
    add_compile_options ("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/mingw_compat.h")
endif ()

add_subdirectory ("${VST3_SDK}" "${PROJECT_BINARY_DIR}/vst3sdk")
smtg_enable_vst3_sdk()

if (MINGW AND NOT MSVC AND TARGET validator)
    target_compile_options (validator PRIVATE -municode)   # le validateur démarre par wmain
    target_link_options (validator PRIVATE -municode)
endif ()

smtg_add_vst3plugin (JSGGuitar
    PACKAGE_NAME "JSG Guitar"
    SOURCES_LIST
        src/factory.cpp
        src/plugin.cpp
        src/plugin_core.cpp
        src/plugin_view.cpp
        "${JSG_SOURCES}/src/server.cpp"
        "${JSG_SOURCES}/src/decode.cpp"
        "${VST3_SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
)

target_include_directories (JSGGuitar PRIVATE src "${JSG_SOURCES}/src" "${JSG_SOURCES}/moteur" "${JSG_SOURCES}/third_party")
target_compile_features (JSGGuitar PRIVATE cxx_std_20)
target_link_libraries (JSGGuitar PRIVATE sdk)

if (WIN32)
    target_include_directories (JSGGuitar PRIVATE "${JSG_SOURCES}/third_party/webview2")
    if (NOT MSVC)
        target_include_directories (JSGGuitar PRIVATE "${JSG_SOURCES}/third_party/webview2/compat")
    endif ()
    target_compile_definitions (JSGGuitar PRIVATE UNICODE _UNICODE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_link_libraries (JSGGuitar PRIVATE ole32 ws2_32 shell32 user32 gdi32 advapi32 uuid)
    if (MSVC)
        target_compile_options (JSGGuitar PRIVATE /utf-8)
    endif ()
else ()
    target_link_libraries (JSGGuitar PRIVATE pthread dl)
endif ()

# Interface dans JSG Guitar.vst3/Contents/Resources/ui
get_target_property (PLUGIN_PACKAGE_PATH JSGGuitar SMTG_PLUGIN_PACKAGE_PATH)
add_custom_command (TARGET JSGGuitar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${JSG_UI}" "${PLUGIN_PACKAGE_PATH}/Contents/Resources/ui"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PLUGIN_PACKAGE_PATH}/Contents/Resources/ui/playbacks"
    COMMENT "Copie de l'interface dans le plug-in")
if (WIN32)
    # Chargeur WebView2 de Microsoft à côté du module
    add_custom_command (TARGET JSGGuitar POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JSG_SOURCES}/third_party/webview2/WebView2Loader.dll" "$<TARGET_FILE_DIR:JSGGuitar>")
endif ()
