@echo off
rem Puts Xbox's ASD entry back from the backup .reg and checks it against the copy, then removes the copy.
setlocal
cd /d "%~dp0"
if "%ROOT%"=="" set "ROOT=HKLM\SOFTWARE\Microsoft\DirectX\ShaderCache"
if "%KEEP%"=="" set "KEEP=HKLM\SOFTWARE\dlssnr-amd-asd-backup"
if "%NAME%"=="" set "NAME=GRTS-XboxContent-7792d9ce-355a-493c-afbd-768f4a77c3b0"
set "LOG=%~dp0restore-log.txt"
set "BAK=%~dp0backup-%NAME%.reg"
echo [%date% %time%] restore %ROOT%\%NAME% > "%LOG%"
if not exist "%BAK%" (echo ABORT: no backup %BAK%>> "%LOG%" & goto done)
reg import "%BAK%" >> "%LOG%" 2>&1 || (echo ABORT: reg import failed>> "%LOG%" & goto done)
reg query "%KEEP%\%NAME%" >nul 2>&1 || (echo WARNING: no copy %KEEP%\%NAME% to compare with: imported from the backup only>> "%LOG%" & goto ok)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify.ps1" "%ROOT%\%NAME%" "%KEEP%\%NAME%" >> "%LOG%" 2>&1 || (echo FAIL: restored key differs from the copy: copy and backup kept>> "%LOG%" & goto done)
reg delete "%KEEP%\%NAME%" /f >> "%LOG%" 2>&1
reg delete "%KEEP%" /f >nul 2>&1
:ok
ren "%BAK%" "restored-%NAME%.reg" >nul 2>&1
echo OK: %ROOT%\%NAME% restored>> "%LOG%"
:done
type "%LOG%"
if "%NOPAUSE%"=="" pause
