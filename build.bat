@echo off
rem Builds pdh.dll and puts the files to copy next to bb-probe.exe in dist\:
rem   dist\pdh.dll, dist\practice-tool\licenses\, dist\README.txt
rem "build.bat zip" also makes dist\BloodbornePracticeTool-v%VERSION%.zip (the release asset).
rem Needs Visual Studio 2022 or its Build Tools with "Desktop development with C++" (MSVC x64 + MASM).
setlocal
set VERSION=0.1.2
cd /d "%~dp0"
set BBS_OUT=pdh.dll
call "%~dp0src\build.bat" || exit /b 1
if exist dist rmdir /s /q dist
mkdir dist\practice-tool\licenses || exit /b 1
copy /y src\pdh.dll dist\pdh.dll >nul || exit /b 1
copy /y src\README_release.txt dist\README.txt >nul || exit /b 1
copy /y LICENSE dist\practice-tool\licenses\LICENSE-BloodbornePracticeTool.txt >nul
copy /y THIRD_PARTY_NOTICES.md dist\practice-tool\licenses\THIRD_PARTY_NOTICES.md >nul
copy /y third_party\fonts\OFL-IMFellEnglishSC.txt dist\practice-tool\licenses\ >nul
copy /y third_party\fonts\OFL-CormorantGaramond.txt dist\practice-tool\licenses\ >nul
copy /y third_party\imgui\src\LICENSE.txt dist\practice-tool\licenses\DearImGui-LICENSE.txt >nul
copy /y third_party\zstd\LICENSE dist\practice-tool\licenses\zstd-LICENSE.txt >nul
copy /y third_party\lz4\lib\LICENSE dist\practice-tool\licenses\lz4-LICENSE.txt >nul
copy /y third_party\xxhash\LICENSE dist\practice-tool\licenses\xxHash-LICENSE.txt >nul
copy /y third_party\vulkan\LICENSE.md dist\practice-tool\licenses\Vulkan-Headers-LICENSE.md >nul
if /i not "%~1"=="zip" goto done
powershell -NoProfile -Command "Compress-Archive -Force -Path 'dist\pdh.dll','dist\practice-tool','dist\README.txt' -DestinationPath 'dist\BloodbornePracticeTool-v%VERSION%.zip'" || exit /b 1
:done
echo.
echo Done: dist\pdh.dll and dist\practice-tool (copy both into ...\bbport-windows\bin)
