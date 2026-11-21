@echo off
rem Compile JSG Guitar.exe avec Visual Studio 2022 (ou plus recent) et CMake.
rem Resultat : build\Release\JSG Guitar.exe (avec le dossier ui)
cd /d "%~dp0"
where cmake >nul 2>nul || (echo CMake introuvable : installez Visual Studio avec "Developpement Desktop en C++" ou CMake. & pause & exit /b 1)
cmake -B build -A x64 || (pause & exit /b 1)
cmake --build build --config Release || (pause & exit /b 1)
echo.
echo Termine : build\Release\JSG Guitar.exe
pause
