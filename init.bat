        @echo off
        chcp 65001 >> nul
        set ELEVATED=
        net.exe session 1>NUL 2>NUL && set ELEVATED=1
        set POWERLINE_DIR=%~dp0
        call %POWERLINE_DIR%\alias.bat
        %POWERLINE_DIR%\_set.bat
