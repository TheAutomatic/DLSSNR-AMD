@echo off
rem Moves Xbox's ASD entry for Minecraft out of ShaderCache for one launch (restore-xbox-admin.bat puts it back).
rem The copy is kept OUTSIDE ShaderCache (a copy inside it would still be a live ASD entry). Logs are appended.
rem Any failure rolls back what this run made (its backup file and copy), the original is only deleted last.
setlocal
cd /d "%~dp0"
if "%ROOT%"=="" set "ROOT=HKLM\SOFTWARE\Microsoft\DirectX\ShaderCache"
if "%KEEP%"=="" set "KEEP=HKLM\SOFTWARE\dlssnr-amd-asd-backup"
if "%NAME%"=="" set "NAME=GRTS-XboxContent-7792d9ce-355a-493c-afbd-768f4a77c3b0"
set "LOG=%~dp0disable-log.txt"
set "BAK=%~dp0backup-%NAME%.reg"
echo.>> "%LOG%"
echo ==== [%date% %time%] disable %ROOT%\%NAME%>> "%LOG%"
if "%NOADMINCHECK%"=="" (
  fltmc >nul 2>&1 || (echo ABORT: not run as administrator: right-click - Run as administrator. Nothing changed.>> "%LOG%" & goto done)
)
reg query "%ROOT%\%NAME%" >nul 2>&1 || (echo ABORT: %ROOT%\%NAME% not found: already disabled? run restore. Nothing changed.>> "%LOG%" & goto done)
reg query "%KEEP%\%NAME%" >nul 2>&1 && (echo ABORT: %KEEP%\%NAME% exists: run restore first. Nothing changed.>> "%LOG%" & goto done)
if exist "%BAK%" (
  echo note: a backup of an earlier failed run was there, the original key is still in place: renamed to stale-%NAME%.reg>> "%LOG%"
  move /y "%BAK%" "%~dp0stale-%NAME%.reg" >> "%LOG%" 2>&1
)
reg export "%ROOT%\%NAME%" "%BAK%" /y >> "%LOG%" 2>&1 || (echo ABORT: reg export failed. Nothing changed.>> "%LOG%" & goto rollback)
if not exist "%BAK%" (echo ABORT: no backup file. Nothing changed.>> "%LOG%" & goto rollback)
echo backup: %BAK%>> "%LOG%"
reg copy "%ROOT%\%NAME%" "%KEEP%\%NAME%" /s /f >> "%LOG%" 2>&1 || (echo ABORT: reg copy failed. Original left in place.>> "%LOG%" & goto rollback)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify.ps1" "%ROOT%\%NAME%" "%KEEP%\%NAME%" >> "%LOG%" 2>&1 || (echo ABORT: the copy differs. Original left in place.>> "%LOG%" & goto rollback)
reg delete "%ROOT%\%NAME%" /f >> "%LOG%" 2>&1 || (echo ABORT: reg delete failed. Original left in place.>> "%LOG%" & goto rollback)
reg query "%ROOT%\%NAME%" >nul 2>&1 && (echo FAIL: the original is still there>> "%LOG%" & goto rollback)
echo OK: %ROOT%\%NAME% disabled (copy in %KEEP%\%NAME%, backup %BAK%)>> "%LOG%"
goto done
:rollback
reg query "%ROOT%\%NAME%" >nul 2>&1 || (echo rollback: the original is gone: importing the backup>> "%LOG%" & reg import "%BAK%" >> "%LOG%" 2>&1)
reg query "%ROOT%\%NAME%" >nul 2>&1 && (
  reg delete "%KEEP%\%NAME%" /f >nul 2>&1
  reg delete "%KEEP%" /f >nul 2>&1
  if exist "%BAK%" del /f "%BAK%"
  echo rollback: done, the original key is in place, this run's backup and copy removed: a rerun starts clean>> "%LOG%"
)
:done
type "%LOG%"
if "%NOPAUSE%"=="" pause
