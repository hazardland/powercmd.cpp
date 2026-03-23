@echo off

for %%a in ("%CD%") do set "PARENT_FOLDER=%%~nxa"
title %PARENT_FOLDER%

set "CURTIME=%TIME: =0%"
set GITBRANCH=
set GITSTATUS=
for /f "tokens=1,* delims= " %%I in ('git.exe status -sb 2^>NUL') do (
    if "%%I"=="##" (
        for /f "tokens=1 delims=." %%K in ("%%J") do set "GITBRANCH=%%K"
    ) else (
        set "GITSTATUS=*"
    )
)

if "%ELEVATED%" == "" (
    if "%GITBRANCH%" == "" (
        prompt $E[38;5;240m[%CURTIME%]$E[38;5;75m%PARENT_FOLDER%$G$E[0m$S
    ) else (
        prompt $E[38;5;240m[%CURTIME%]$E[38;5;75m%PARENT_FOLDER%$E[0m[$E[38;5;229m%GITBRANCH%%GITSTATUS%$E[0m]$E[38;5;75m$G$E[0m$S
    )
) else (
    if "%GITBRANCH%" == "" (
        prompt $E[38;5;240m[%CURTIME%]$E[38;5;203m%PARENT_FOLDER%$G$E[0m$S
    ) else (
        prompt $E[38;5;240m[%CURTIME%]$E[38;5;203m%PARENT_FOLDER%$E[0m[$E[38;5;229m%GITBRANCH%%GITSTATUS%$E[0m]$E[38;5;203m$G$E[0m$S
    )
)
