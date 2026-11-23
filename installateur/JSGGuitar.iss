; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 Jérôme Soler — JSG Guitar
;
; Installateur Windows de JSG Guitar (Inno Setup 6) : le logiciel, le plug-in VST3 et,
; s'ils sont présents, les playbacks JSG.
;
; Le plus simple : lancer tout_compiler.bat, qui compile tout puis appelle ce script.
; À la main : remplir le dossier « fichiers » comme ci-dessous, puis
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" JSGGuitar.iss
;
;   fichiers\app\        JSG Guitar.exe, WebView2Loader.dll, ui\
;   fichiers\vst3\       JSG Guitar.vst3\  (le dossier complet du plug-in)
;   fichiers\docs\       guides et licences (.txt)
;   fichiers\playbacks\  (facultatif) les MP3 du pack « JSG Playbacks » et LICENCE.txt

#define AppName      "JSG Guitar"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define AppPublisher "Jérôme Soler Group"
#define AppURL       "https://jeromesolergroup.com"
#ifndef Fichiers
  #define Fichiers   "fichiers"
#endif
#define AvecPlaybacks DirExists(AddBackslash(SourcePath) + Fichiers + "\playbacks")

[Setup]
AppId={{4A3BE070-19DD-400C-A5A6-B3914FB4303F}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppCopyright=© 2026 Jérôme Soler — logiciel libre (GPL v3)
VersionInfoVersion={#AppVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
VersionInfoDescription=Installation de {#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Le plug-in va dans C:\Program Files\Common Files\VST3 : droits administrateur nécessaires
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
InfoBeforeFile=avant.txt
OutputDir=sortie
OutputBaseFilename=JSG Guitar Setup
SetupIconFile=..\sources\res\jsg.ico
UninstallDisplayIcon={app}\JSG Guitar.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Types]
Name: "complete"; Description: "Installation complète"
Name: "logiciel"; Description: "Logiciel seul (sans le plug-in)"
Name: "plugin";   Description: "Plug-in VST3 seul"
Name: "perso";    Description: "Personnalisée"; Flags: iscustom

[Components]
Name: "app";  Description: "JSG Guitar — le logiciel (ampli, pédales, lecteur, enregistrement)"; Types: complete logiciel perso
Name: "vst3"; Description: "Plug-in VST3 — pour Reaper, Cubase, Studio One, FL Studio, Ableton Live…"; Types: complete plugin perso
#if AvecPlaybacks
Name: "app\playbacks"; Description: "Playbacks JSG — 8 morceaux du groupe sans la guitare (licence CC BY-NC 4.0)"; Types: complete logiciel perso
#endif

[Tasks]
Name: "bureau"; Description: "Créer un raccourci sur le Bureau"; Components: app

[Dirs]
; Dossier du plug-in en « système » pour que Windows affiche son icône (desktop.ini)
Name: "{commoncf64}\VST3\JSG Guitar.vst3"; Attribs: system; Components: vst3

[Files]
; Le logiciel
Source: "{#Fichiers}\app\*"; DestDir: "{app}"; Components: app; Flags: ignoreversion recursesubdirs createallsubdirs
; Le plug-in VST3 (dossier complet)
Source: "{#Fichiers}\vst3\JSG Guitar.vst3\*"; Excludes: "desktop.ini"; DestDir: "{commoncf64}\VST3\JSG Guitar.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Fichiers}\vst3\JSG Guitar.vst3\desktop.ini"; DestDir: "{commoncf64}\VST3\JSG Guitar.vst3"; Components: vst3; Attribs: hidden system; Flags: ignoreversion skipifsourcedoesntexist
; Guides et licences : toujours installés
Source: "{#Fichiers}\docs\*"; DestDir: "{app}"; Flags: ignoreversion
#if AvecPlaybacks
; Les playbacks JSG (licence CC BY-NC 4.0)
Source: "{#Fichiers}\playbacks\*.mp3"; DestDir: "{app}\ui\playbacks"; Components: app\playbacks; Flags: ignoreversion
Source: "{#Fichiers}\playbacks\LICENCE.txt"; DestDir: "{app}"; DestName: "LICENCE-PLAYBACKS.txt"; Components: app\playbacks; Flags: ignoreversion skipifsourcedoesntexist
#endif

[Icons]
Name: "{group}\JSG Guitar"; Filename: "{app}\JSG Guitar.exe"; Components: app
Name: "{group}\Guide du logiciel"; Filename: "{app}\GUIDE-UTILISATEUR.txt"; Components: app
Name: "{group}\Guide du plug-in VST3"; Filename: "{app}\GUIDE-PLUGIN.txt"; Components: vst3
Name: "{group}\Site du Jérôme Soler Group"; Filename: "{#AppURL}"
Name: "{group}\Désinstaller JSG Guitar"; Filename: "{uninstallexe}"
Name: "{autodesktop}\JSG Guitar"; Filename: "{app}\JSG Guitar.exe"; Tasks: bureau

[Run]
Filename: "{app}\JSG Guitar.exe"; Description: "Lancer JSG Guitar"; Components: app; Flags: nowait postinstall skipifsilent
Filename: "{app}\GUIDE-PLUGIN.txt"; Description: "Lire le guide du plug-in VST3"; Components: vst3; Flags: shellexec postinstall skipifsilent unchecked

; La désinstallation retire le logiciel et le plug-in. Les réglages, presets et
; enregistrements restent dans %LOCALAPPDATA%\JSG Guitar et Musique\JSG Guitar.

[Code]
const
  WebView2Cle = 'Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  WebView2Page = 'https://developer.microsoft.com/microsoft-edge/webview2/';

function VersionValide(const V: String): Boolean;
begin
  Result := (V <> '') and (V <> '0.0.0.0');
end;

{ WebView2 Runtime (le moteur d'affichage de l'interface) : présent sur Windows 10/11 à jour }
function WebView2Present(): Boolean;
var
  V: String;
begin
  Result := (RegQueryStringValue(HKLM32, 'SOFTWARE\' + WebView2Cle, 'pv', V) and VersionValide(V))
         or (RegQueryStringValue(HKLM64, 'SOFTWARE\' + WebView2Cle, 'pv', V) and VersionValide(V))
         or (RegQueryStringValue(HKCU, 'Software\' + WebView2Cle, 'pv', V) and VersionValide(V));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep = ssPostInstall) and (not WizardSilent()) and (not WebView2Present()) then
  begin
    if MsgBox('JSG Guitar affiche son interface avec « Microsoft Edge WebView2 Runtime », ' +
              'qui ne semble pas installé sur ce PC (Windows pas à jour ?).' + #13#10#13#10 +
              'Sans lui, le logiciel s''ouvre dans une fenêtre Edge et le plug-in VST3 ' +
              'ne peut pas afficher sa fenêtre.' + #13#10#13#10 +
              'Ouvrir la page de téléchargement de Microsoft (gratuit) ?',
              mbConfirmation, MB_YESNO) = IDYES then
      ShellExec('open', WebView2Page, '', '', SW_SHOWNORMAL, ewNoWait, Code);
  end;
end;
