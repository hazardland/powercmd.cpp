@echo off
taskkill /f /im pcmd.exe >nul 2>&1
set /p MINOR=<version.txt
set /a MINOR=%MINOR%+1
(echo %MINOR%)>version.txt
echo Building Power CMD v0.0.%MINOR%...
g++ pcmd.cpp -o pcmd.exe -DVERSION_MINOR=%MINOR% -ladvapi32 -lshell32
if %errorlevel% == 0 (
    echo Done: Power CMD v0.0.%MINOR%
) else (
    set /a MINOR=%MINOR%-1
    (echo %MINOR%)>version.txt
    echo Build failed, version rolled back.
)
