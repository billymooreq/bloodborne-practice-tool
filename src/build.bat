@echo off
rem Builds bbstate.dll (also the one-file install: copied as pdh.dll next to bb-probe.exe) with MSVC (VS 2022 Build Tools):
rem MASM thunks + pdh.dll proxy exports, C (savestates, practice), C++ (overlay: Dear ImGui + Vulkan), fonts as resources.
rem Extra C flags: set BBS_CFLAGS (e.g. /DBBS_SHIFT_TEST). Output name: BBS_OUT.
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
if "%BBS_OUT%"=="" set BBS_OUT=bbstate.dll
set TP=..\third_party
set IG=%TP%\imgui\src
if not exist obj mkdir obj
if not exist obj\ig mkdir obj\ig
if not exist obj\libs\zstd.lib call "%~dp0build_libs.bat" || exit /b 1
rem rva.h comes from the symbol table of bb-probe.exe 1.9.5 (tools\coff_symbols.py); without it the checked-in rva.h is used
if exist ..\tools\bbprobe_195_symbols.txt (python gen_rva.py >nul || exit /b 1)
ml64 /nologo /c /Foobj\thunks.obj thunks.asm || exit /b 1
ml64 /nologo /c /Foobj\proxy_pdh.obj proxy_pdh.asm || exit /b 1
rc /nologo /fo obj\overlay.res overlay.rc || exit /b 1
if not exist obj\ig\imgui_impl_vulkan.obj (
  cl /nologo /c /O2 /MT /DNDEBUG /W1 /EHsc /std:c++17 /DVK_NO_PROTOTYPES /DIMGUI_IMPL_VULKAN_NO_PROTOTYPES /DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS /I%IG% /I%TP%\vulkan /Foobj\ig\ %IG%\imgui.cpp %IG%\imgui_draw.cpp %IG%\imgui_tables.cpp %IG%\imgui_widgets.cpp %IG%\backends\imgui_impl_vulkan.cpp || exit /b 1
)
cl /nologo /c /O2 /MT /DNDEBUG /W3 /EHsc /std:c++17 /DVK_NO_PROTOTYPES /DIMGUI_IMPL_VULKAN_NO_PROTOTYPES /DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS /I%IG% /I%TP%\vulkan /Foobj\ ov_vk.cpp ov_ui.cpp || exit /b 1
cl /nologo /c /O2 /MT /DNDEBUG /W3 %BBS_CFLAGS% /Foobj\ bbstate.c store.c vsync.c api.c meta.c keys.c practice.c proxy.c sha256.c || exit /b 1
link /nologo /DLL /OUT:%BBS_OUT% /DEF:proxy_pdh.def /MAP:obj\bbstate.map obj\bbstate.obj obj\store.obj obj\vsync.obj obj\api.obj obj\meta.obj obj\keys.obj obj\practice.obj obj\proxy.obj obj\sha256.obj ^
  obj\ov_vk.obj obj\ov_ui.obj obj\ig\imgui.obj obj\ig\imgui_draw.obj obj\ig\imgui_tables.obj obj\ig\imgui_widgets.obj obj\ig\imgui_impl_vulkan.obj ^
  obj\thunks.obj obj\proxy_pdh.obj obj\overlay.res ^
  delayimp.lib /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:winmm.dll /DELAYLOAD:shell32.dll user32.lib shell32.lib winmm.lib || exit /b 1
echo BUILD OK
