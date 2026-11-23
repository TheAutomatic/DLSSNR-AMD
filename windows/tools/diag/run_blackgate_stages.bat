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
if not defined NR_LOG_PATH set "NR_LOG_PATH=%CD%\dlssnr-amd.log"
if not exist "logs" mkdir "logs"
if not exist "logs" exit /b 1
echo Stages 4, 5 and 6: %runs% runs each. Exit the game after observing the 3D scene.
echo Stage 7 is not implemented; it will be skipped, never launched as stage 0.
echo Log source: "%NR_LOG_PATH%"
echo Keep [Log] Enabled=1 and ClearOnStart=1 in dlssnr-amd.ini.
>>"logs\results.txt" echo Session %DATE% %TIME%, runs=%runs%
for %%S in (4 5 6 7) do (
    if %%S==7 (
        echo Stage 7 skipped: the network build itself creates frame-sized images.
        >>"logs\results.txt" echo stage7 SKIPPED: requires a separate image-free network build path
    ) else (
        for /l %%R in (1,1,%runs%) do call :run %%S %%R
    )
)
exit /b 0

:run
set "NR_DIAG_STAGE=%1"
echo.
echo Stage %1, run %2 of %runs%
start "" /wait "%CD%\Game.exe"
set "game_exit=%ERRORLEVEL%"
set "log_status=MISSING"
if exist "%NR_LOG_PATH%" (
    copy /y "%NR_LOG_PATH%" "logs\stage%1_run%2.log" >nul
    if not errorlevel 1 set "log_status=copied"
)
if not "%log_status%"=="copied" echo WARNING: no log copied. Check NR_LOG_PATH and [Log] settings.
choice /c YN /n /m "black? y/n "
if errorlevel 2 (set "black=n") else (set "black=y")
>>"logs\results.txt" echo stage%1_run%2 black=%black% exit=%game_exit% log=%log_status%
exit /b 0

:usage
echo Usage: run_blackgate_stages.bat [N from 1 to 999, default 3]
exit /b 1
