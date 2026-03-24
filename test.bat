@echo off
g++ pcmd_test.cpp -o pcmd_test.exe -DVERSION_MINOR=0 -ladvapi32 -lshell32
if %errorlevel% == 0 (
    pcmd_test.exe
) else (
    echo Compile failed.
)
