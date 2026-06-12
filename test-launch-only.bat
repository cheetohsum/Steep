@echo off
setlocal

REM ==========================================================================
REM  Launch-only variant: runs the already-bundled steep.exe with logging.
REM  No rebuild, no bundling. Use after test-session.bat for fast iterations.
REM ==========================================================================

set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "SOURCE_ROOT=C:\SourceCode\rawtherapee"
set "RELEASE_DIR_UNIX=~/build-win/Release"

for /f "tokens=*" %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
set "LOGNAME=test-%STAMP%.log"
set "LOGFILE_WIN=%SOURCE_ROOT%\%LOGNAME%"
set "LOGFILE_UNIX=/c/SourceCode/rawtherapee/%LOGNAME%"

echo Launching steep.exe; log -^> %LOGFILE_WIN%
echo.
"%BASH%" -lc "export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; cd %RELEASE_DIR_UNIX% && ./steep.exe >'%LOGFILE_UNIX%' 2>&1"
set "EXITCODE=%ERRORLEVEL%"

echo.
echo steep.exe exited with code %EXITCODE%
echo Log: %LOGFILE_WIN%
pause
