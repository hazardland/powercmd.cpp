@echo off
taskkill /f /im pcmd.exe >nul 2>&1
set /p MINOR=<version.txt
set /a MINOR=%MINOR%+1
(echo %MINOR%)>version.txt

set DEMO_FLAG=
if /i "%1"=="demo" set DEMO_FLAG=-DDEMO

echo Building pcmd.exe v0.0.%MINOR%...
g++ pcmd.cpp -o pcmd.exe -DVERSION_MINOR=%MINOR% %DEMO_FLAG% -ladvapi32 -lshell32
if %errorlevel% == 0 (
    echo Done: pcmd.exe v0.0.%MINOR%
) else (
    set /a MINOR=%MINOR%-1
    (echo %MINOR%)>version.txt
    echo Build failed, version rolled back.
)
