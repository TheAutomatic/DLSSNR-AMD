@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "runs=%~1"
if not defined runs set "runs=3"
for /f "delims=0123456789" %%A in ("%runs%") do goto usage
if "%runs:~0,1%"=="0" goto usage
if not "%runs:~3%"=="" goto usage
if not exist "Game.exe" (
    echo Run this batch from the folder containing Game.exe.
    exit /b 1
)
set "NR_DIAG_STAGE="
if not defined NR_LOG_PATH set "NR_LOG_PATH=%CD%\dlssnr-amd.log"
if not exist "logs" mkdir "logs"
if not exist "logs" exit /b 1
echo Fixes baseline, device-nobda, device, robust, all: %runs% runs each, normal processing.
echo Exit the game after observing the 3D scene.
echo Log source: "%NR_LOG_PATH%"
echo Keep [Log] Enabled=1 and ClearOnStart=1 in dlssnr-amd.ini.
if defined NR_NO_BDA echo WARNING: inherited NR_NO_BDA may override the device mode's BDA selection.
>>"logs\results.txt" echo Session %DATE% %TIME%, fixes, runs=%runs%
for %%F in (baseline device-nobda device robust all) do (
    for /l %%R in (1,1,%runs%) do call :run %%F %%R
)
exit /b 0

:run
set "NR_DIAG_FIX=%1"
if "%1"=="baseline" set "NR_DIAG_FIX="
set "NR_DIAG_STAGE="
echo.
echo Fix %1, run %2 of %runs%
start "" /wait "%CD%\Game.exe"
set "game_exit=%ERRORLEVEL%"
set "log_status=MISSING"
if exist "%NR_LOG_PATH%" (
    copy /y "%NR_LOG_PATH%" "logs\fix_%1_run%2.log" >nul
    if not errorlevel 1 set "log_status=copied"
)
if not "%log_status%"=="copied" echo WARNING: no log copied. Check NR_LOG_PATH and [Log] settings.
choice /c YN /n /m "black? y/n "
if errorlevel 2 (set "black=n") else (set "black=y")
>>"logs\results.txt" echo fix_%1_run%2 black=%black% exit=%game_exit% log=%log_status%
exit /b 0

:usage
echo Usage: run_blackgate_fixes.bat [N from 1 to 999, default 3]
exit /b 1
