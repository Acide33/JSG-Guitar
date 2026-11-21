@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem JSG Guitar : compile le logiciel et le plug-in VST3, puis fabrique l'installateur.
rem
rem Il faut : Visual Studio 2022 ("Developpement Desktop en C++"), git, Inno Setup 6.
rem Resultat : installateur\sortie\JSG Guitar Setup.exe
rem
rem Facultatif :
rem  - playbacks JSG dans l'installateur : copiez le dossier "playbacks" du pack
rem    "JSG Playbacks" ici (installateur\playbacks\*.mp3 + LICENCE.txt)
rem  - signature Certum : ouvrez SimplySign Desktop, puis lancez ce script avec
rem      set JSG_CERT=Votre nom tel qu'il est dans le certificat
rem      tout_compiler.bat
setlocal EnableExtensions
cd /d "%~dp0"
set "SRC=%~dp0..\sources"
set "UI=%~dp0..\ui"
set "OUT=%~dp0fichiers"

where cmake >nul 2>nul || (echo CMake introuvable : installez Visual Studio avec "Developpement Desktop en C++". & goto erreur)

echo.
echo === 1/4  Logiciel JSG Guitar.exe ===
cmake -S "%SRC%" -B "%SRC%\build" -A x64 || goto erreur
cmake --build "%SRC%\build" --config Release || goto erreur

echo.
echo === 2/4  Plug-in JSG Guitar.vst3 ===
if not exist "%SRC%\plugin\vst3sdk\CMakeLists.txt" (
  where git >nul 2>nul || (echo git introuvable : installez Git pour Windows. & goto erreur)
  echo Telechargement du kit VST3 de Steinberg - licence MIT...
  git clone --recursive --depth 1 https://github.com/steinbergmedia/vst3sdk.git "%SRC%\plugin\vst3sdk" || goto erreur
)
cmake -S "%SRC%\plugin" -B "%SRC%\plugin\build" -A x64 || goto erreur
cmake --build "%SRC%\plugin\build" --config Release || goto erreur

echo.
echo === 3/4  Fichiers a installer ===
if exist "%OUT%" rmdir /s /q "%OUT%"
robocopy "%SRC%\build\Release" "%OUT%\app" "JSG Guitar.exe" WebView2Loader.dll /NJH /NJS /NFL /NDL >nul
if errorlevel 8 goto erreur
robocopy "%UI%" "%OUT%\app\ui" /E /XF *.mp3 /NJH /NJS /NFL /NDL >nul
if errorlevel 8 goto erreur
robocopy "%SRC%\plugin\build\VST3\Release\JSG Guitar.vst3" "%OUT%\vst3\JSG Guitar.vst3" /E /NJH /NJS /NFL /NDL >nul
if errorlevel 8 goto erreur
mkdir "%OUT%\docs"
copy /y "%~dp0..\GUIDE-UTILISATEUR.txt" "%OUT%\docs\" >nul || goto erreur
copy /y "%~dp0..\GUIDE-PLUGIN.txt" "%OUT%\docs\" >nul || goto erreur
copy /y "%~dp0..\LICENSE" "%OUT%\docs\LICENCE-GPLv3.txt" >nul || goto erreur
copy /y "%~dp0..\LICENCE-CONTENUS.md" "%OUT%\docs\LICENCE-CONTENUS.txt" >nul || goto erreur
copy /y "%~dp0..\LICENCES-TIERS.md" "%OUT%\docs\LICENCES-TIERS.txt" >nul || goto erreur
if exist "%~dp0playbacks\*.mp3" (
  echo Playbacks JSG trouves : ajoutes a l'installateur.
  robocopy "%~dp0playbacks" "%OUT%\playbacks" *.mp3 LICENCE.txt /NJH /NJS /NFL /NDL >nul
  if errorlevel 8 goto erreur
) else (
  echo Pas de playbacks dans installateur\playbacks : installateur sans les morceaux.
)

if defined JSG_CERT call :signtool || goto erreur
if defined JSG_CERT call :signer "%OUT%\app\JSG Guitar.exe" "%OUT%\vst3\JSG Guitar.vst3\Contents\x86_64-win\JSG Guitar.vst3" || goto erreur

echo.
echo === 4/4  Installateur ===
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (echo Inno Setup 6 introuvable : installez-le depuis jrsoftware.org. & goto erreur)
"%ISCC%" /Q "%~dp0JSGGuitar.iss" || goto erreur

if defined JSG_CERT call :signer "%~dp0sortie\JSG Guitar Setup.exe" || goto erreur

echo.
echo Termine : installateur\sortie\JSG Guitar Setup.exe
if not defined CI pause
exit /b 0

:signer
echo Signature : %*
"%SIGNTOOL%" sign /n "%JSG_CERT%" /fd sha256 /tr http://time.certum.pl /td sha256 %*
exit /b %errorlevel%

:signtool
set "SIGNTOOL="
for /f "delims=" %%f in ('where signtool 2^>nul') do if not defined SIGNTOOL set "SIGNTOOL=%%f"
if not defined SIGNTOOL for /f "delims=" %%f in ('dir /b /s /o:n "%ProgramFiles(x86)%\Windows Kits\10\bin\signtool.exe" 2^>nul ^| findstr /i "\\x64\\"') do set "SIGNTOOL=%%f"
if not defined SIGNTOOL (echo signtool introuvable : installez le "Windows SDK" avec Visual Studio. & exit /b 1)
exit /b 0

:erreur
echo.
echo ECHEC : voir le message ci-dessus.
if not defined CI pause
exit /b 1
