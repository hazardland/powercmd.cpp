@echo off

@REM Powerline internal aliases - do not remove
doskey git=%POWERLINE_DIR%\_git.bat $*
doskey cd=%POWERLINE_DIR%\_cd.bat $*
doskey ls=%POWERLINE_DIR%\_ls.bat $*
doskey cls=%POWERLINE_DIR%\_cls.bat
doskey clear=%POWERLINE_DIR%\_cls.bat

@REM Add your custom aliases below
doskey reload=call %POWERLINE_DIR%\init.bat
