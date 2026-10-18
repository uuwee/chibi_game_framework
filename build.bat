
@echo off
setlocal

cd /d "%~dp0"

if not exist ".build\bin\build.exe" (
    call bootstrap.bat
    if errorlevel 1 exit /b 1
)

if exist ".build\bin\build-next.exe" (
    del /q ".build\bin\build-next.exe"
    if errorlevel 1 exit /b 1
)

".build\bin\build.exe" %*

set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    exit /b %RESULT%
)

if exist ".build\bin\build-next.exe" (
    move /Y ".build\bin\build-next.exe" ".build\bin\build.exe" >nul

    if errorlevel 1 (
        echo [ERROR] Cannot update build runner.
        exit /b 1
    )

    echo [SUCCESS] Build runner updated!
)

exit /b 0
