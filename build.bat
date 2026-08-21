@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
rc /nologo /i src /fo build\vde.res src\vde.rc || exit /b 1
cl /nologo /utf-8 /EHsc /W3 /std:c++14 src\vde.cpp src\picker_trace.cpp build\vde.res /Fe:build\vde.exe /Fo:build\ || exit /b 1
echo Built build\vde.exe
