@echo off
rem Build the offline PSDB generator (asd_psdb_gen.exe) with MSVC (x64 only).
rem
rem Environment overrides:
rem   ASD_SDK_DIR      Agility SDK "build\native" directory: contains include\d3d12compiler.h
rem                    and bin\x64\D3D12StateObjectCompiler.dll.
rem                    Default: third_party\agility\build\native
rem   ASD_PLUGIN_DIR   directory holding amdxc_compiler.dll.
rem                    Default: third_party\amd-plugin\build\native\bin\x64
rem   ASD_DXC          dxc.exe path. Default: newest Windows Kits 10 x64 dxc.exe.
rem   ASD_VCVARS       vcvars64.bat or vcvarsall.bat. Default: vswhere, then a scan
rem                    of "%ProgramFiles(x86)%\Microsoft Visual Studio".
rem   ASD_OBJ_DIR      intermediate directory. Default: build\obj
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "ARCH=x64"
if not "%~1"=="" set "ARCH=%~1"
if /I not "%ARCH%"=="x64" (
    echo only x64 is supported
    exit /b 1
)

if not defined ASD_SDK_DIR set "ASD_SDK_DIR=%ROOT%\third_party\agility\build\native"
if not defined ASD_PLUGIN_DIR set "ASD_PLUGIN_DIR=%ROOT%\third_party\amd-plugin\build\native\bin\x64"
if not defined ASD_OBJ_DIR set "ASD_OBJ_DIR=%ROOT%\build\obj"
if not defined ASD_DXC call :find_dxc
if not defined ASD_DXC (
    echo dxc.exe not found; set ASD_DXC
    exit /b 1
)

if not exist "%ASD_SDK_DIR%\include\d3d12compiler.h" (
    echo ASD_SDK_DIR has no include\d3d12compiler.h: "%ASD_SDK_DIR%"
    exit /b 1
)
if not exist "%ASD_SDK_DIR%\bin\x64\D3D12StateObjectCompiler.dll" (
    echo ASD_SDK_DIR has no bin\x64\D3D12StateObjectCompiler.dll: "%ASD_SDK_DIR%"
    exit /b 1
)
if not exist "%ASD_PLUGIN_DIR%\amdxc_compiler.dll" (
    echo ASD_PLUGIN_DIR has no amdxc_compiler.dll: "%ASD_PLUGIN_DIR%"
    exit /b 1
)
if not exist "%ASD_DXC%" (
    echo dxc.exe not found: "%ASD_DXC%"
    exit /b 1
)

call :find_vcvars
if errorlevel 1 exit /b 1
if not exist "%ASD_OBJ_DIR%" mkdir "%ASD_OBJ_DIR%"
if not exist "%ROOT%\build\bin" mkdir "%ROOT%\build\bin"

echo [build] vcvars=%ASD_VCVARS%
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%ASD_SDK_DIR%\include" /I"%ROOT%\src" ^
   "%ROOT%\src\asd_probe.cpp" ^
   /Fe:"%ROOT%\build\bin\asd_psdb_gen.exe" /Fo:"%ASD_OBJ_DIR%\\" /Fd:"%ASD_OBJ_DIR%\asd_psdb_gen.pdb" ^
   /link d3d12.lib dxguid.lib
if errorlevel 1 exit /b 1
if exist "%ASD_SDK_DIR%\bin\x64\D3D12Core.dll" copy /y "%ASD_SDK_DIR%\bin\x64\D3D12Core.dll" "%ROOT%\build\bin\" >nul
if not exist "%ROOT%\build\bin\D3D12Core.dll" (
    echo warning: D3D12Core.dll not copied; place the Agility 1.619.5 D3D12Core.dll next to asd_psdb_gen.exe
)
echo [build] %ROOT%\build\bin\asd_psdb_gen.exe
endlocal
exit /b 0

:find_dxc
call :scan_dxc "%ProgramFiles(x86)%\Windows Kits\10\bin"
call :scan_dxc "%ProgramFiles%\Windows Kits\10\bin"
exit /b 0

:scan_dxc
if defined ASD_DXC exit /b 0
if not exist "%~1" exit /b 0
for /f "delims=" %%v in ('dir /b /ad /o-n "%~1" 2^>nul') do if not defined ASD_DXC if exist "%~1\%%v\x64\dxc.exe" set "ASD_DXC=%~1\%%v\x64\dxc.exe"
exit /b 0

:find_vcvars
if defined ASD_VCVARS goto :vcvars_set
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call :take_vcvars "%%i\VC\Auxiliary\Build"
if defined ASD_VCVARS goto :vcvars_set
for /d %%a in ("%ProgramFiles(x86)%\Microsoft Visual Studio\*") do for /d %%b in ("%%~a\*") do call :take_vcvars "%%~b\VC\Auxiliary\Build"
if defined ASD_VCVARS goto :vcvars_set
echo no Visual Studio with C++ tools found; set ASD_VCVARS to vcvars64.bat or vcvarsall.bat
exit /b 1

:take_vcvars
if defined ASD_VCVARS exit /b 0
if exist "%~1\vcvars64.bat" (
    set "ASD_VCVARS=%~1\vcvars64.bat"
    set "ASD_VCVARS_ALL="
    exit /b 0
)
if exist "%~1\vcvarsall.bat" (
    set "ASD_VCVARS=%~1\vcvarsall.bat"
    set "ASD_VCVARS_ALL=1"
    exit /b 0
)
exit /b 0

:vcvars_set
if not exist "%ASD_VCVARS%" (
    echo vcvars not found: "%ASD_VCVARS%"
    exit /b 1
)
if defined ASD_VCVARS_ALL (
    set "TOOLSVER="
    for %%x in ("%ASD_VCVARS%") do set "BUILDDIR=%%~dpx"
    for /f "delims=" %%v in ('dir /b /ad /o-n "!BUILDDIR!..\..\Tools\MSVC" 2^>nul') do if not defined TOOLSVER if exist "!BUILDDIR!..\..\Tools\MSVC\%%v\bin\HostX64\x64\cl.exe" set "TOOLSVER=%%v"
    if defined TOOLSVER (
        call "%ASD_VCVARS%" x64 -vcvars_ver=!TOOLSVER! >nul
    ) else (
        call "%ASD_VCVARS%" x64 >nul
    )
) else (
    call "%ASD_VCVARS%" >nul
)
exit /b %errorlevel%
