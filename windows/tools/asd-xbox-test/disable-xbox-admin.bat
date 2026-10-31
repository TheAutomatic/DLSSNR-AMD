@echo off
rem Moves Xbox's ASD entry for Minecraft out of ShaderCache for one launch (restore-xbox-admin.bat puts it back).
rem The copy is kept OUTSIDE ShaderCache (a copy inside it would still be a live ASD entry).
setlocal
cd /d "%~dp0"
if "%ROOT%"=="" set "ROOT=HKLM\SOFTWARE\Microsoft\DirectX\ShaderCache"
if "%KEEP%"=="" set "KEEP=HKLM\SOFTWARE\dlssnr-amd-asd-backup"
if "%NAME%"=="" set "NAME=GRTS-XboxContent-7792d9ce-355a-493c-afbd-768f4a77c3b0"
set "LOG=%~dp0disable-log.txt"
set "BAK=%~dp0backup-%NAME%.reg"
echo [%date% %time%] disable %ROOT%\%NAME% > "%LOG%"
reg query "%ROOT%\%NAME%" >nul 2>&1 || (echo ABORT: %ROOT%\%NAME% not found>> "%LOG%" & goto done)
reg query "%KEEP%\%NAME%" >nul 2>&1 && (echo ABORT: %KEEP%\%NAME% exists already: run restore first>> "%LOG%" & goto done)
if exist "%BAK%" (echo ABORT: %BAK% exists already: run restore first>> "%LOG%" & goto done)
reg export "%ROOT%\%NAME%" "%BAK%" /y >> "%LOG%" 2>&1 || (echo ABORT: reg export failed>> "%LOG%" & goto done)
if not exist "%BAK%" (echo ABORT: no backup file>> "%LOG%" & goto done)
echo backup: %BAK%>> "%LOG%"
reg copy "%ROOT%\%NAME%" "%KEEP%\%NAME%" /s /f >> "%LOG%" 2>&1 || (echo ABORT: reg copy failed>> "%LOG%" & goto done)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify.ps1" "%ROOT%\%NAME%" "%KEEP%\%NAME%" >> "%LOG%" 2>&1 || (echo ABORT: the copy differs, original left in place>> "%LOG%" & reg delete "%KEEP%\%NAME%" /f >nul 2>&1 & goto done)
reg delete "%ROOT%\%NAME%" /f >> "%LOG%" 2>&1 || (echo ABORT: reg delete failed>> "%LOG%" & goto done)
reg query "%ROOT%\%NAME%" >nul 2>&1 && (echo FAIL: still there>> "%LOG%" & goto done)
echo OK: %ROOT%\%NAME% disabled (copy in %KEEP%\%NAME%, backup %BAK%)>> "%LOG%"
:done
type "%LOG%"
if "%NOPAUSE%"=="" pause
