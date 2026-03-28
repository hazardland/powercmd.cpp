@echo off
taskkill /f /im zcmd.exe >nul 2>&1
set /p MINOR=<version.txt
set /a MINOR=%MINOR%+1
(echo %MINOR%)>version.txt

echo Building zcmd.exe v0.0.%MINOR%...
g++ zcmd.cpp -o zcmd.exe -DVERSION_MINOR=%MINOR% -ladvapi32 -lshell32 -liphlpapi -lpsapi -static
if %errorlevel% == 0 (
    echo Done: zcmd.exe v0.0.%MINOR%
) else (
    set /a MINOR=%MINOR%-1
    (echo %MINOR%)>version.txt
    echo Build failed, version rolled back.
)
