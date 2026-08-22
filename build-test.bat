@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /utf-8 /EHsc /W3 /bigobj /std:c++14 /I src tests\vdtest.cpp src\picker_trace.cpp /Fe:build\vdtest.exe /Fo:build\ || exit /b 1
build\vdtest.exe
