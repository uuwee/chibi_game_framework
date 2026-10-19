@echo off
@setlocal

cd /d "%~dp0"

if not exist ".build" mkdir ".build"
if not exist ".build\obj" mkdir ".build\obj"
if not exist ".build\bin" mkdir ".build\bin"

echo [BOOTSTRAP] Compiling build runner...

cl.exe /nologo /W4 /std:clatest /MD /c buildsys\build.c /Fo.build\obj\build.c.obj
cl.exe /nologo /W4 /std:clatest /MD /c buildsys\main.c /Fo.build\obj\main.c.obj

if errorlevel 1 exit /b 1

echo [BOOTSTRAP] Linking build runner...

link.exe /NOLOGO .build\obj\build.c.obj  .build\obj\main.c.obj /OUT:.build\bin\build.exe

if errorlevel 1 exit /b 1

echo [SUCCESS] Bootstrap completed!

exit /b 0
