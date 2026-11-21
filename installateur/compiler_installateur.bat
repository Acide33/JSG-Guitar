@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Fabrique "JSG Guitar Setup.exe" a partir du dossier "fichiers" deja rempli
rem (logiciel, plug-in, guides, playbacks). Il faut seulement Inno Setup 6.3 ou plus recent.
rem Pour tout recompiler depuis les sources : tout_compiler.bat
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "fichiers\app\JSG Guitar.exe" (echo Dossier "fichiers" incomplet : lancez plutot tout_compiler.bat. & goto erreur)
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (echo Inno Setup 6 introuvable : installez-le gratuitement depuis jrsoftware.org, puis relancez. & goto erreur)
echo Fabrication de l'installateur...
"%ISCC%" /Q "%~dp0JSGGuitar.iss" || goto erreur
echo.
echo Termine : sortie\JSG Guitar Setup.exe
explorer "%~dp0sortie"
if not defined CI pause
exit /b 0
:erreur
echo.
echo ECHEC : voir le message ci-dessus.
if not defined CI pause
exit /b 1
