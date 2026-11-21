@echo off
rem Compile JSG Guitar.vst3 avec Visual Studio 2022 (ou plus recent), CMake et git.
rem Resultat : build\VST3\Release\JSG Guitar.vst3
cd /d "%~dp0"
where cmake >nul 2>nul || (echo CMake introuvable : installez Visual Studio avec "Developpement Desktop en C++". & pause & exit /b 1)
if not exist vst3sdk\CMakeLists.txt (
  echo Telechargement du kit VST3 de Steinberg...
  git clone --recursive --depth 1 https://github.com/steinbergmedia/vst3sdk.git vst3sdk || (echo git introuvable ou pas de connexion. & pause & exit /b 1)
)
cmake -B build -A x64 || (pause & exit /b 1)
cmake --build build --config Release || (pause & exit /b 1)
echo.
echo Termine : build\VST3\Release\JSG Guitar.vst3
echo A copier dans C:\Program Files\Common Files\VST3
pause
