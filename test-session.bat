@echo off
setlocal enabledelayedexpansion

REM ==========================================================================
REM  Steep / RawTherapee test session launcher
REM  - Rebuilds RelWithDebInfo via ninja (MSYS2 MINGW64)
REM  - Runs bundle-win.sh to sync DLLs + models into the Release dir
REM  - Launches steep.exe with stdout + stderr redirected to a timestamped log
REM ==========================================================================

set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "SOURCE_ROOT=C:\SourceCode\rawtherapee"
set "BUILD_DIR_UNIX=~/build-win/RelWithDebInfo"
set "RELEASE_DIR_UNIX=~/build-win/Release"

REM --- timestamp for log file (yyyymmdd-HHMMSS) ---
for /f "tokens=*" %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
set "LOGNAME=test-%STAMP%.log"
set "LOGFILE_WIN=%SOURCE_ROOT%\%LOGNAME%"
set "LOGFILE_UNIX=/c/SourceCode/rawtherapee/%LOGNAME%"

echo ==========================================================================
echo  Steep test session - %STAMP%
echo  Log: %LOGFILE_WIN%
echo ==========================================================================
echo.

if not exist "%BASH%" (
  echo ERROR: MSYS2 bash not found at %BASH%
  echo Adjust MSYS2_ROOT in this script if your install is elsewhere.
  pause
  exit /b 1
)

REM --- Step 1: rebuild via ninja ---
echo [1/3] Rebuilding RelWithDebInfo via ninja ...
"%BASH%" -lc "export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; cd %BUILD_DIR_UNIX% 2>/dev/null || { echo 'Build dir not found: %BUILD_DIR_UNIX%'; exit 2; }; ninja install"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo Build step failed ^(exit %RC%^). Fix errors above and re-run.
  pause
  exit /b %RC%
)

echo.
REM --- Step 2: bundle DLLs + models into Release ---
echo [2/3] Running bundle-win.sh to sync DLLs into Release ...
"%BASH%" -lc "export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; cd /c/SourceCode/rawtherapee && bash bundle-win.sh"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo Bundle step failed ^(exit %RC%^). Aborting launch.
  pause
  exit /b %RC%
)

echo.
REM --- Step 3: launch, capturing stdout + stderr ---
echo [3/3] Launching steep.exe ...
echo       Reproduce the crash now. Close the app when done.
echo       Log   -^> %LOGFILE_WIN%
echo.
"%BASH%" -lc "export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1; cd %RELEASE_DIR_UNIX% && ./steep.exe >'%LOGFILE_UNIX%' 2>&1"
set "EXITCODE=%ERRORLEVEL%"

echo.
echo ==========================================================================
echo  steep.exe exited with code %EXITCODE%
echo  Log: %LOGFILE_WIN%
echo ==========================================================================
echo.
echo Tip: if it crashed, tail the log and share the '=== CRASH ===' section.
pause
