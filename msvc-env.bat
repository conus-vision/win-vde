@echo off
rem msvc-env.bat - puts the x64 MSVC toolchain on PATH for the build scripts.
rem Call it from a script that already ran setlocal; it does not build anything.
rem An active x64 Developer Command Prompt is reused as is. Otherwise vswhere
rem picks the newest Visual Studio 2017 or later that has the C++ x64 build
rem tools, in any edition (Community, Professional, Enterprise, Build Tools).
rem The script uses no labels, so it also works when checked out with LF endings.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 0

set "VDE_VSDIR="
set "VDE_VSWHERE_DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VDE_VSWHERE_DIR%\vswhere.exe" set "VDE_VSWHERE_DIR="
rem vswhere runs from its own folder so that no quoted path containing the
rem parentheses of "Program Files (x86)" has to pass through FOR /F. The .\
rem prefix keeps it found when NoDefaultCurrentDirectoryInExePath is set.
if defined VDE_VSWHERE_DIR pushd "%VDE_VSWHERE_DIR%"
if defined VDE_VSWHERE_DIR for /f "usebackq delims=" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VDE_VSDIR=%%i"
if defined VDE_VSWHERE_DIR popd
if defined VDE_VSDIR if exist "%VDE_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" call "%VDE_VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 0

echo Visual Studio 2017 or later with the C++ x64 build tools was not found. 1>&2
echo Install the "Desktop development with C++" workload, or run the build 1>&2
echo from an x64 Native Tools Command Prompt. 1>&2
exit /b 1
