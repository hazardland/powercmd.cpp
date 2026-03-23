        @echo off

        for %%a in ("%CD%") do set "PARENT_FOLDER=%%~nxa"
        title %PARENT_FOLDER%

        @REM set "PARENT_FOLDER=%CD%"

        set GITBRANCH=
        for /f %%I in ('git.exe branch --show-current 2^> NUL') do set GITBRANCH=%%I
        @REM for /f %%I in ('git rev-parse --abbrev-ref HEAD 2^> NUL') do set GITBRANCH=%%I
        @REM for /f "tokens=2" %%I in ('git.exe branch 2^> NUL ^| findstr /b "* "') do set GITBRANCH=%%I

        @REM if "%GITBRANCH%" NEQ "" (
        @REM     set GITSTATUS=
        @REM     for /f %%I in ('git status --porcelain') do set GITSTATUS=*
        @REM )

        if "%ELEVATED%" == "" (
            if "%GITBRANCH%" == "" (
                prompt $E[30;46m$S$E[0m$E[30;46m%PARENT_FOLDER%$S$E[0m$E[36m$E[0m$S
            ) else (
                prompt $E[30;46m$S$E[0m$E[30;46m%PARENT_FOLDER%$S$E[0m$E[36;43m$S$E[0m$E[30;43m%GITBRANCH%$S$E[0m$E[33m$E[0m$S
            )
        ) else (
            if "%GITBRANCH%" == "" (
                prompt $E[30;41m$S$E[0m$E[30;41m%PARENT_FOLDER%$S$E[0m$E[31m$E[0m$S
            ) else (
                prompt $E[30;41m$S$E[0m$E[30;41m%PARENT_FOLDER%$S$E[0m$E[31;43m$S$E[0m$E[30;43m%GITBRANCH%$S$E[0m$E[33m$E[0m$S
            )
        )
