@echo off
rem Builds lz4.lib and zstd.lib (MSVC, /MT) from third_party sources into obj\libs.
setlocal
if defined VCINSTALLDIR goto have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo Visual Studio 2022 or its Build Tools are not installed & exit /b 1
for %%a in ("%VSWHERE%") do set "VSWHERE=%%~sa"
for /f "delims=" %%i in ('%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR echo The C++ build tools (MSVC x64) are not installed & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
cd /d "%~dp0"
set TP=..\third_party
if not exist obj\libs\lz4 mkdir obj\libs\lz4
if not exist obj\libs\zstd mkdir obj\libs\zstd
cl /nologo /c /O2 /MT /DNDEBUG /W1 /Foobj\libs\lz4\ %TP%\lz4\lib\lz4.c %TP%\lz4\lib\lz4hc.c || exit /b 1
lib /nologo /out:obj\libs\lz4.lib obj\libs\lz4\*.obj || exit /b 1
cl /nologo /c /O2 /MT /DNDEBUG /W1 /DZSTD_DISABLE_ASM /DZSTD_MULTITHREAD=0 /I%TP%\zstd\lib /Foobj\libs\zstd\ %TP%\zstd\lib\common\*.c %TP%\zstd\lib\compress\*.c %TP%\zstd\lib\decompress\*.c || exit /b 1
lib /nologo /out:obj\libs\zstd.lib obj\libs\zstd\*.obj || exit /b 1
echo LIBS OK
