import os
import sys
import re
import shutil
import zipfile
from pathlib import Path

REPO_ROOT = Path("/home/kenny/src/DLSSNR-AMD-rdna3")

def determine_version():
    if len(sys.argv) > 1 and sys.argv[1].strip():
        arg_ver = sys.argv[1].strip().lstrip("v")
        return arg_ver
    
    candidates = []
    ver_file = REPO_ROOT / ".package_version"
    if ver_file.exists():
        v = ver_file.read_text().strip().lstrip("v")
        if v:
            candidates.append(v)
            
    dl_dir = Path("/mnt/c/Users/Kenny/Downloads")
    if dl_dir.exists():
        for p in dl_dir.glob("RDNA3*DLSSNR-GameTest-v*.zip"):
            m = re.search(r"v(\d+\.\d+\.\d+)", p.name)
            if m:
                candidates.append(m.group(1))

    art_dir = REPO_ROOT / "artifacts/windows"
    if art_dir.exists():
        for p in art_dir.glob("RDNA3*DLSSNR-GameTest-v*"):
            m = re.search(r"v(\d+\.\d+\.\d+)", p.name)
            if m:
                candidates.append(m.group(1))

    if not candidates:
        return "1.0.3"

    def parse_ver(v):
        return tuple(int(x) for x in v.split("."))

    latest = max(candidates, key=parse_ver)
    maj, min_, pat = parse_ver(latest)
    return f"{maj}.{min_}.{pat + 1}"

VERSION = determine_version()
print(f"============================================================")
print(f"  Packaging DLSS-NR AMD (RDNA3 / RDNA3.5) Game Test v{VERSION}")
print(f"============================================================")

(REPO_ROOT / ".package_version").write_text(VERSION + "\n")

PKG_DIR = REPO_ROOT / f"artifacts/windows/RDNA3&3.5-DLSSNR-GameTest-v{VERSION}"
OPTI_ZIP = REPO_ROOT / "artifacts/ref/downloads/OptiScaler-NR-v0.8.91.zip"
BENCH_ZIP = Path("/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-RDNA3-RDNA3.5-Benchmark-v1.1.zip")
OPTISCALER_BIN = REPO_ROOT / "artifacts/windows/optiscaler"
ACO_DATA = REPO_ROOT / "windows/data/aco/rdna3"
TMP_ACO_SHADERS = Path("/tmp/test-aco-shaders")

print("[1/8] Cleaning and creating package directory...")
if PKG_DIR.exists():
    shutil.rmtree(PKG_DIR)
PKG_DIR.mkdir(parents=True, exist_ok=True)

print("[2/8] Extracting OptiScaler base...")
with zipfile.ZipFile(OPTI_ZIP) as z:
    for info in z.infolist():
        name = info.filename.replace("\\", "/")
        if name.startswith("docs/") or name.startswith("images/") or name.startswith("tests/"):
            continue
        if name in ["setup_windows.bat", "setup_linux.sh", "SHA256SUMS.txt", "CONTRIBUTING.md", "Features.md", "Spoofing.md", "Config.md", "INSTALL-DLSSNR.md", "!! EXTRACT ALL FILES TO GAME FOLDER !!"]:
            continue
        target = PKG_DIR / name
        if name.endswith("/") or info.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with z.open(info) as src, open(target, "wb") as dst:
            dst.write(src.read())

# OptiScaler.dll -> dxgi.dll as primary loader
shutil.copy2(PKG_DIR / "OptiScaler.dll", PKG_DIR / "dxgi.dll")

print("[3/8] Copying compiled RDNA3/3.5 DLLs...")
shutil.copy2(OPTISCALER_BIN / "_nvngx.dll", PKG_DIR / "dlssnr_core.dll")
shutil.copy2(OPTISCALER_BIN / "nvngx.dll_dlssnr.dll", PKG_DIR / "nvngx.dll_dlssnr.dll")
shutil.copy2(OPTISCALER_BIN / "nvngx_dlssnr.dll", PKG_DIR / "nvngx_dlssnr.dll")

dlssnr_amd_dir = PKG_DIR / "dlssnr-amd"
dlssnr_amd_dir.mkdir(parents=True, exist_ok=True)
shutil.copy2(OPTISCALER_BIN / "nr_present_probe.exe", dlssnr_amd_dir / "nr_present_probe.exe")

print("[4/8] Extracting model weights...")
if BENCH_ZIP.exists():
    with zipfile.ZipFile(BENCH_ZIP) as z:
        for name in ["DLSSNR-AMD-RDNA3-Benchmark-v1.0/dlssnr.bin", "DLSSNR-AMD-RDNA3-Benchmark-v1.0/dlssnr-small.bin"]:
            base = Path(name).name
            target = dlssnr_amd_dir / base
            print(f"  Extracting {base}...")
            with z.open(name) as src, open(target, "wb") as dst:
                dst.write(src.read())
else:
    # Fallback to existing package or tmp
    fallback_sources = [
        REPO_ROOT / "artifacts/windows/RDNA3&3.5-DLSSNR-GameTest-v1.0.3/dlssnr-amd",
        Path("/mnt/c/tmp/dlssnr-amd"),
        REPO_ROOT / "artifacts/windows/DLSSNR-AMD-RDNA3-GameTest-v1.0/dlssnr-amd"
    ]
    for src_dir in fallback_sources:
        if (src_dir / "dlssnr-small.bin").exists() and (src_dir / "dlssnr.bin").exists():
            print(f"  Copying weights from {src_dir}...")
            shutil.copy2(src_dir / "dlssnr-small.bin", dlssnr_amd_dir / "dlssnr-small.bin")
            shutil.copy2(src_dir / "dlssnr.bin", dlssnr_amd_dir / "dlssnr.bin")
            break
    else:
        raise FileNotFoundError("Could not find dlssnr.bin and dlssnr-small.bin in benchmark zip or fallbacks")

print("[5/8] Assembling Dual-Architecture ACO bundles & native shaders...")
aco_dir = dlssnr_amd_dir / "aco"
aco_rdna3_dir = aco_dir / "rdna3"
aco_rdna4_dir = aco_dir / "rdna4"
(aco_rdna3_dir / "records").mkdir(parents=True, exist_ok=True)
(aco_rdna3_dir / "shaders").mkdir(parents=True, exist_ok=True)
(aco_rdna3_dir / "shells").mkdir(parents=True, exist_ok=True)
(aco_rdna3_dir / "shell").mkdir(parents=True, exist_ok=True)
(aco_rdna4_dir / "records").mkdir(parents=True, exist_ok=True)

# 1. RDNA3 ACO bundle
for rec in (ACO_DATA / "records").glob("*.nrp"):
    shutil.copy2(rec, aco_rdna3_dir / "records" / rec.name)

if TMP_ACO_SHADERS.exists():
    shutil.copytree(TMP_ACO_SHADERS, aco_rdna3_dir / "shaders", dirs_exist_ok=True)
else:
    shutil.copytree(ACO_DATA / "shaders", aco_rdna3_dir / "shaders", dirs_exist_ok=True)

shutil.copytree(ACO_DATA / "shells", aco_rdna3_dir / "shells", dirs_exist_ok=True)
shutil.copytree(ACO_DATA / "shells", aco_rdna3_dir / "shell", dirs_exist_ok=True)
shutil.copy2(ACO_DATA / "shell-aliases.txt", aco_rdna3_dir / "shell-aliases.txt")

# 2. RDNA4 ACO bundle
ACO_RDNA4_DATA = REPO_ROOT / "windows/data/aco/rdna4"
if not ACO_RDNA4_DATA.exists():
    ACO_RDNA4_DATA = REPO_ROOT / "windows/data/aco"
(aco_rdna4_dir / "records").mkdir(parents=True, exist_ok=True)
for rec in (ACO_RDNA4_DATA / "records").glob("*.nrp"):
    shutil.copy2(rec, aco_rdna4_dir / "records" / rec.name)
if (ACO_RDNA4_DATA / "shaders").exists():
    shutil.copytree(ACO_RDNA4_DATA / "shaders", aco_rdna4_dir / "shaders", dirs_exist_ok=True)
elif Path("/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-Vulkan-Windows-0.0.7-preview-x86_64/dlssnr-amd/aco/shaders").exists():
    shutil.copytree(Path("/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-Vulkan-Windows-0.0.7-preview-x86_64/dlssnr-amd/aco/shaders"), aco_rdna4_dir / "shaders", dirs_exist_ok=True)
if (ACO_RDNA4_DATA / "shell-aliases.txt").exists():
    shutil.copy2(ACO_RDNA4_DATA / "shell-aliases.txt", aco_rdna4_dir / "shell-aliases.txt")

# 3. Flat fallback for backwards compatibility
(aco_dir / "records").mkdir(parents=True, exist_ok=True)
for rec in (ACO_DATA / "records").glob("*.nrp"):
    shutil.copy2(rec, aco_dir / "records" / rec.name)
shutil.copytree(aco_rdna3_dir / "shaders", aco_dir / "shaders", dirs_exist_ok=True)
shutil.copytree(aco_rdna3_dir / "shells", aco_dir / "shells", dirs_exist_ok=True)
shutil.copytree(aco_rdna3_dir / "shells", aco_dir / "shell", dirs_exist_ok=True)
shutil.copy2(ACO_DATA / "shell-aliases.txt", aco_dir / "shell-aliases.txt")

# 4. Native shaders: dual architecture
shaders_dir = dlssnr_amd_dir / "shaders"
shaders_rdna3 = shaders_dir / "rdna3"
shaders_rdna4 = shaders_dir / "rdna4"
shutil.copytree(aco_rdna3_dir / "shaders", shaders_rdna3, dirs_exist_ok=True)

RDNA4_UPSTREAM_SHADERS = Path("/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-Vulkan-Windows-0.0.7-preview-x86_64/dlssnr-amd/shaders")
if RDNA4_UPSTREAM_SHADERS.exists():
    shutil.copytree(RDNA4_UPSTREAM_SHADERS, shaders_rdna4, dirs_exist_ok=True)

# Default shaders root
shutil.copytree(aco_rdna3_dir / "shaders", shaders_dir, dirs_exist_ok=True)

print("[6/8] Configuring OptiScaler.ini and dlssnr-amd.ini...")
# Configure OptiScaler.ini
opti_ini = PKG_DIR / "OptiScaler.ini"
lines = opti_ini.read_text(encoding="utf-8").splitlines()
out_lines = []
section = ""
for line in lines:
    sline = line.strip()
    if sline.startswith("[") and sline.endswith("]"):
        section = sline[1:-1]
        out_lines.append(line)
        continue
    if section == "DlssNr" and sline.startswith("Enabled="):
        out_lines.append("Enabled=true")
    elif section == "DlssNr" and sline.startswith("WhitePointSource="):
        out_lines.append("WhitePointSource=1")
    elif section == "Libraries" and sline.startswith("NvngxPath="):
        out_lines.append(r"NvngxPath=.\dlssnr_core.dll")
    elif section == "Upscalers" and sline.startswith("Dx11Upscaler="):
        out_lines.append("Dx11Upscaler=ffx_12")
    elif section == "Log" and sline.startswith("LogToFile="):
        out_lines.append("LogToFile=true")
    elif section == "Log" and sline.startswith("LogLevel="):
        out_lines.append("LogLevel=1")
    else:
        out_lines.append(line)
opti_ini.write_text("\r\n".join(out_lines) + "\r\n", encoding="utf-8")

# Configure dlssnr-amd.ini
if Path("/tmp/ini_out/dlssnr-amd-optiscaler.ini").exists():
    src_ini = Path("/tmp/ini_out/dlssnr-amd-optiscaler.ini").read_text(encoding="utf-8")
elif (REPO_ROOT / "ini/dlssnr-amd-optiscaler.ini").exists():
    src_ini = (REPO_ROOT / "ini/dlssnr-amd-optiscaler.ini").read_text(encoding="utf-8")
else:
    src_ini = (REPO_ROOT / "artifacts/windows/DLSSNR-AMD-RDNA3-GameTest-v1.0/dlssnr-amd/dlssnr-amd.ini").read_text(encoding="utf-8")
(PKG_DIR / "dlssnr-amd.ini").write_text(src_ini.replace("\n", "\r\n"), encoding="utf-8")
(dlssnr_amd_dir / "dlssnr-amd.ini").write_text(src_ini.replace("\n", "\r\n"), encoding="utf-8")

print("[7/8] Generating Installer batch scripts & Documentation...")

# 1. 一键安装到游戏.bat (Pure 7-bit ASCII wrapper)
install_bat = r'''@echo off
setlocal
cd /d "%~dp0"
title DLSS-NR AMD (RDNA3 / RDNA3.5) Installer

where powershell.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install_game.ps1" %*
    if errorlevel 1 pause
    exit /b %ERRORLEVEL%
)

:: Pure ASCII Batch Fallback if powershell is not found
set "TARGET_DIR=%~1"
if "%TARGET_DIR%"=="" (
    echo Drag and drop the game exe or game directory onto this batch file.
    set /p "TARGET_DIR=Game path: "
)
if "%TARGET_DIR%"=="" exit /b 1
set "TARGET_DIR=%TARGET_DIR:"=%"
if exist "%TARGET_DIR%\" (
    set "GAME_DIR=%TARGET_DIR%"
) else (
    for %%I in ("%TARGET_DIR%") do set "GAME_DIR=%%~dpI"
)
if "%GAME_DIR:~-1%"=="\" set "GAME_DIR=%GAME_DIR:~0,-1%"

if exist "%GAME_DIR%\dxgi.dll" (
    if not exist "%GAME_DIR%\dlssnr_backup" mkdir "%GAME_DIR%\dlssnr_backup"
    copy /y "%GAME_DIR%\dxgi.dll" "%GAME_DIR%\dlssnr_backup\dxgi.dll.bak" >nul
)
xcopy /e /y /i "%~dp0OptiScaler" "%GAME_DIR%\OptiScaler" >nul
xcopy /e /y /i "%~dp0dlssnr-amd" "%GAME_DIR%\dlssnr-amd" >nul
copy /y "%~dp0dxgi.dll" "%GAME_DIR%\" >nul
copy /y "%~dp0OptiScaler.dll" "%GAME_DIR%\" >nul
copy /y "%~dp0dlssnr_core.dll" "%GAME_DIR%\" >nul
copy /y "%~dp0nvngx.dll_dlssnr.dll" "%GAME_DIR%\" >nul
copy /y "%~dp0nvngx_dlssnr.dll" "%GAME_DIR%\" >nul
copy /y "%~dp0dlssnr-amd.ini" "%GAME_DIR%\" >nul
copy /y "%~dp0OptiScaler.ini" "%GAME_DIR%\" >nul
echo [OK] Installation completed successfully.
pause
'''

# 2. install_game.ps1 (Rich PowerShell UI with UTF-8 BOM)
install_game_ps1 = r'''param([string]$Target = '', [switch]$NoPause)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host "       DLSS-NR AMD (RDNA3 / RDNA3.5 / RDNA4) 全架构整合测试包 v__VERSION__" -ForegroundColor Cyan
Write-Host "  支持硬件: RX 9000 / RX 7000 / 8060S / 890M / 780M / 混合双显卡全系" -ForegroundColor Cyan
Write-Host "  核心特性: 单 DLL 跨架构运行时动态派发 (LUID 渲染设备精准绑定)" -ForegroundColor Cyan
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host ""

# 1. 解析目标路径 (支持拖拽传参 或 双击时弹出原生文件选择对话框)
if (-not $Target) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = "请选择游戏主程序 (例如 Cyberpunk2077.exe)"
        $dialog.Filter = "游戏主程序 (*.exe)|*.exe|所有文件 (*.*)|*.*"
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $Target = $dialog.FileName
        }
    } catch { }
}

if (-not $Target) {
    Write-Host "请将游戏主程序 (.exe) 或游戏文件夹拖动到批处理上，"
    Write-Host "或者直接在下方输入游戏完整路径："
    $Target = Read-Host "游戏路径"
}

if (-not $Target) {
    Write-Host "[提示] 未指定游戏路径，安装已取消。" -ForegroundColor Yellow
    Read-Host "按 Enter 键退出..."
    exit 0
}

$Target = $Target.Trim('"')
if (Test-Path -LiteralPath $Target -PathType Leaf) {
    $gameDir = Split-Path -Parent (Resolve-Path -LiteralPath $Target).Path
} elseif (Test-Path -LiteralPath $Target -PathType Container) {
    $gameDir = (Resolve-Path -LiteralPath $Target).Path
} else {
    Write-Host "[错误] 路径不存在: $Target" -ForegroundColor Red
    Read-Host "按 Enter 键退出..."
    exit 1
}

# 智能识别：检测是否为虚幻引擎 (Unreal Engine) 或含有 Win64/x64 二级目录的游戏
# 虚幻引擎（如剑星 Stellar Blade、黑神话等）根目录的 .exe 只是几百KB的启动器，真正的渲染主程序在 Binaries\Win64\ 下
$realExe = $null
if (Test-Path (Join-Path $gameDir "Engine")) {
    $candidates = Get-ChildItem -LiteralPath $gameDir -Filter "*Shipping.exe" -Recurse -Depth 4 -ErrorAction SilentlyContinue
    if (-not $candidates) {
        $candidates = Get-ChildItem -LiteralPath $gameDir -Filter "*.exe" -Recurse -Depth 4 -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "Binaries[\\/]Win64" -and $_.Length -gt 1MB }
    }
    if ($candidates) { $realExe = $candidates[0].FullName }
}
if (-not $realExe) {
    $binX64 = Join-Path $gameDir "bin\x64"
    if (Test-Path $binX64) {
        $binCandidates = Get-ChildItem -LiteralPath $binX64 -Filter "*.exe" -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -gt 1MB }
        if ($binCandidates) { $realExe = $binCandidates[0].FullName }
    }
}
if ($realExe) {
    $realGameDir = Split-Path -Parent $realExe
    if ($realGameDir -ne $gameDir) {
        Write-Host "[智能识别] 检测到虚幻引擎/独立渲染主程序：" -ForegroundColor Yellow
        Write-Host "          $realExe" -ForegroundColor Yellow
        Write-Host "          自动将安装路径重定向至真实目录: $realGameDir" -ForegroundColor Green
        $gameDir = $realGameDir
    }
}

# 2. 权限检测与自动提权 (例如目标在 C:\Program Files 等受保护目录)
$probe = Join-Path $gameDir ("dlssnr-probe-" + [guid]::NewGuid().ToString("N"))
$needAdmin = $false
try {
    [System.IO.File]::WriteAllText($probe, "probe")
    Remove-Item -LiteralPath $probe -Force
} catch {
    $needAdmin = $true
}

if ($needAdmin) {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "[提示] 目标目录需要管理员权限，正在请求提升权限..." -ForegroundColor Yellow
        $proc = Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -STA -File `"$PSCommandPath`" `"$Target`"" -PassThru -Wait
        exit $proc.ExitCode
    }
}

Write-Host "[1/4] 目标安装目录: $gameDir" -ForegroundColor Green
Write-Host ""

# 3. 备份原有 dxgi.dll
$dxgiPath = Join-Path $gameDir "dxgi.dll"
$backupDir = Join-Path $gameDir "dlssnr_backup"
if (Test-Path -LiteralPath $dxgiPath) {
    Write-Host "[2/4] 发现原有 dxgi.dll，备份至 dlssnr_backup\dxgi.dll.bak..." -ForegroundColor Yellow
    if (-not (Test-Path -LiteralPath $backupDir)) {
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $dxgiPath -Destination (Join-Path $backupDir "dxgi.dll.bak") -Force
} else {
    Write-Host "[2/4] 未发现原有 dxgi.dll，无需备份。" -ForegroundColor Gray
}

# 4. 复制组件到游戏目录
Write-Host "[3/4] 正在复制 OptiScaler 与 DLSS-NR 整合组件到游戏目录..." -ForegroundColor Green

$itemsToCopy = @(
    "OptiScaler",
    "dlssnr-amd",
    "dxgi.dll",
    "OptiScaler.dll",
    "dlssnr_core.dll",
    "nvngx.dll_dlssnr.dll",
    "nvngx_dlssnr.dll",
    "dlssnr-amd.ini",
    "OptiScaler.ini"
)

foreach ($item in $itemsToCopy) {
    $srcPath = Join-Path $here $item
    if (Test-Path -LiteralPath $srcPath) {
        $destPath = Join-Path $gameDir $item
        if (Test-Path -LiteralPath $srcPath -PathType Container) {
            if (-not (Test-Path -LiteralPath $destPath)) {
                New-Item -ItemType Directory -Path $destPath -Force | Out-Null
            }
            Copy-Item -Path (Join-Path $srcPath "*") -Destination $destPath -Recurse -Force
        } else {
            Copy-Item -LiteralPath $srcPath -Destination $destPath -Force
        }
        Write-Host "  -> 已部署: $item" -ForegroundColor Gray
    } else {
        Write-Host "  -> [跳过] 未找到源文件: $item" -ForegroundColor DarkGray
    }
}

# 验证核心文件存在
$vitalFiles = @("dxgi.dll", "OptiScaler.dll", "dlssnr_core.dll", "dlssnr-amd.ini")
$allOk = $true
foreach ($vf in $vitalFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $gameDir $vf))) {
        Write-Host "[警告] 关键文件缺失: $vf" -ForegroundColor Red
        $allOk = $false
    }
}

Write-Host ""
if ($allOk) {
    Write-Host "=======================================================================" -ForegroundColor Green
    Write-Host "                         【安装部署成功！】" -ForegroundColor Green
    Write-Host "=======================================================================" -ForegroundColor Green
} else {
    Write-Host "=======================================================================" -ForegroundColor Red
    Write-Host "                  【安装完成，但部分关键文件可能缺失】" -ForegroundColor Red
    Write-Host "=======================================================================" -ForegroundColor Red
}
Write-Host ""
Write-Host "【快捷键与使用提示】：" -ForegroundColor Cyan
Write-Host " 1. 启动游戏：正常从 Steam / Epic 或快捷方式启动游戏即可。"
Write-Host " 2. 游戏内设置：确保开启 DLSS (超分辨率) 或 FSR。"
Write-Host " 3. OptiScaler 菜单：游戏中按 [Insert] 键呼出（配置超分/帧生成）。"
Write-Host " 4. DLSS-NR 菜单：游戏中按 [End] 键呼出（配置神经网络降噪/画质风格）。"
Write-Host " 5. 快速切换网络：按 [Ctrl+F11] 可在极限性能 (CC_SMALL_FP8) 与 原版全重之间秒切对比。"
Write-Host " 6. 对比截图：按 [F5] 自动截取开启/关闭降噪的对比原图。"
Write-Host ""
Write-Host "如需卸载，运行本包内的【一键卸载.bat】即可。"
Write-Host "======================================================================="
Write-Host ""
if (-not $NoPause) { Read-Host "请按 Enter 键退出..." }
'''

# 3. 一键卸载.bat (Pure 7-bit ASCII wrapper)
uninstall_bat = r'''@echo off
setlocal
cd /d "%~dp0"
title DLSS-NR AMD Uninstaller

where powershell.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0uninstall_game.ps1" %*
    if errorlevel 1 pause
    exit /b %ERRORLEVEL%
)

:: Pure ASCII Batch Fallback if powershell is not found
set "TARGET_DIR=%~1"
if "%TARGET_DIR%"=="" (
    echo Drag and drop the game exe or game directory to uninstall.
    set /p "TARGET_DIR=Game path: "
)
if "%TARGET_DIR%"=="" exit /b 1
set "TARGET_DIR=%TARGET_DIR:"=%"
if exist "%TARGET_DIR%\" (
    set "GAME_DIR=%TARGET_DIR%"
) else (
    for %%I in ("%TARGET_DIR%") do set "GAME_DIR=%%~dpI"
)
if "%GAME_DIR:~-1%"=="\" set "GAME_DIR=%GAME_DIR:~0,-1%"

del /f /q "%GAME_DIR%\dxgi.dll" 2>nul
del /f /q "%GAME_DIR%\OptiScaler.dll" 2>nul
del /f /q "%GAME_DIR%\dlssnr_core.dll" 2>nul
del /f /q "%GAME_DIR%\nvngx.dll_dlssnr.dll" 2>nul
del /f /q "%GAME_DIR%\nvngx_dlssnr.dll" 2>nul
del /f /q "%GAME_DIR%\OptiScaler.ini" 2>nul
del /f /q "%GAME_DIR%\dlssnr-amd.ini" 2>nul
del /f /q "%GAME_DIR%\OptiScaler.log" 2>nul
del /f /q "%GAME_DIR%\dlssnr-amd.log" 2>nul
if exist "%GAME_DIR%\OptiScaler" rmdir /s /q "%GAME_DIR%\OptiScaler"
if exist "%GAME_DIR%\dlssnr-amd" rmdir /s /q "%GAME_DIR%\dlssnr-amd"
if exist "%GAME_DIR%\dlssnr_backup\dxgi.dll.bak" (
    copy /y "%GAME_DIR%\dlssnr_backup\dxgi.dll.bak" "%GAME_DIR%\dxgi.dll" >nul
    rmdir /s /q "%GAME_DIR%\dlssnr_backup"
)
echo [OK] Uninstalled successfully.
pause
'''

# 4. uninstall_game.ps1 (Rich PowerShell UI with UTF-8 BOM)
uninstall_game_ps1 = r'''param([string]$Target = '')

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

Write-Host "=======================================================================" -ForegroundColor Yellow
Write-Host "                   DLSS-NR AMD 一键卸载与还原工具" -ForegroundColor Yellow
Write-Host "=======================================================================" -ForegroundColor Yellow
Write-Host ""

if (-not $Target) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = "请选择要卸载的游戏主程序 (例如 Cyberpunk2077.exe)"
        $dialog.Filter = "游戏主程序 (*.exe)|*.exe|所有文件 (*.*)|*.*"
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $Target = $dialog.FileName
        }
    } catch { }
}

if (-not $Target) {
    Write-Host "请输入要卸载的游戏主程序路径或游戏所在目录："
    $Target = Read-Host "游戏路径"
}

if (-not $Target) {
    Write-Host "[提示] 未指定游戏路径，卸载已取消。" -ForegroundColor Yellow
    Read-Host "按 Enter 键退出..."
    exit 0
}

$Target = $Target.Trim('"')
if (Test-Path -LiteralPath $Target -PathType Leaf) {
    $gameDir = Split-Path -Parent (Resolve-Path -LiteralPath $Target).Path
} elseif (Test-Path -LiteralPath $Target -PathType Container) {
    $gameDir = (Resolve-Path -LiteralPath $Target).Path
} else {
    Write-Host "[错误] 路径不存在: $Target" -ForegroundColor Red
    Read-Host "按 Enter 键退出..."
    exit 1
}

# 智能识别：检测是否为虚幻引擎或含有 Win64/x64 二级目录的游戏
$realExe = $null
if (Test-Path (Join-Path $gameDir "Engine")) {
    $candidates = Get-ChildItem -LiteralPath $gameDir -Filter "*Shipping.exe" -Recurse -Depth 4 -ErrorAction SilentlyContinue
    if (-not $candidates) {
        $candidates = Get-ChildItem -LiteralPath $gameDir -Filter "*.exe" -Recurse -Depth 4 -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "Binaries[\\/]Win64" -and $_.Length -gt 1MB }
    }
    if ($candidates) { $realExe = $candidates[0].FullName }
}
if (-not $realExe) {
    $binX64 = Join-Path $gameDir "bin\x64"
    if (Test-Path $binX64) {
        $binCandidates = Get-ChildItem -LiteralPath $binX64 -Filter "*.exe" -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -gt 1MB }
        if ($binCandidates) { $realExe = $binCandidates[0].FullName }
    }
}
if ($realExe) {
    $realGameDir = Split-Path -Parent $realExe
    if ($realGameDir -ne $gameDir) {
        Write-Host "[智能识别] 自动定位到真实卸载目录: $realGameDir" -ForegroundColor Yellow
        $gameDir = $realGameDir
    }
}

# 权限检测与自动提权
$probe = Join-Path $gameDir ("dlssnr-probe-" + [guid]::NewGuid().ToString("N"))
$needAdmin = $false
try {
    [System.IO.File]::WriteAllText($probe, "probe")
    Remove-Item -LiteralPath $probe -Force
} catch {
    $needAdmin = $true
}

if ($needAdmin) {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "[提示] 目标目录需要管理员权限，正在请求提升权限..." -ForegroundColor Yellow
        $proc = Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -STA -File `"$PSCommandPath`" `"$Target`"" -PassThru -Wait
        exit $proc.ExitCode
    }
}

Write-Host "正在从 `"$gameDir`" 移除整合文件..." -ForegroundColor Yellow

$filesToRemove = @(
    "dxgi.dll", "OptiScaler.dll", "dlssnr_core.dll",
    "nvngx.dll_dlssnr.dll", "nvngx_dlssnr.dll",
    "OptiScaler.ini", "dlssnr-amd.ini",
    "OptiScaler.log", "dlssnr-amd.log"
)

foreach ($f in $filesToRemove) {
    $p = Join-Path $gameDir $f
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
        Write-Host "  -> 已删除: $f" -ForegroundColor Gray
    }
}

$dirsToRemove = @("OptiScaler", "dlssnr-amd")
foreach ($d in $dirsToRemove) {
    $p = Join-Path $gameDir $d
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "  -> 已删除文件夹: $d" -ForegroundColor Gray
    }
}

# 还原备份
$bakFile = Join-Path $gameDir "dlssnr_backup\dxgi.dll.bak"
$bakDir = Join-Path $gameDir "dlssnr_backup"
if (Test-Path -LiteralPath $bakFile) {
    Write-Host "正在恢复原有备份 dxgi.dll..." -ForegroundColor Green
    Copy-Item -LiteralPath $bakFile -Destination (Join-Path $gameDir "dxgi.dll") -Force
    Remove-Item -LiteralPath $bakDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "  -> 原有 dxgi.dll 已成功还原！" -ForegroundColor Green
}

Write-Host ""
Write-Host "=======================================================================" -ForegroundColor Green
Write-Host "                 【卸载完成】游戏目录已恢复原样。" -ForegroundColor Green
Write-Host "=======================================================================" -ForegroundColor Green
Write-Host ""
Read-Host "按 Enter 键退出..."
'''

# 3. README_使用与实测指南.txt
readme_txt = r'''===============================================================================
       DLSS-NR AMD (RDNA3 / RDNA3.5 / RDNA4) 全架构实机集成测试包 v__VERSION__
===============================================================================

【适用硬件与架构支持】
  - AMD Radeon RX 9000 全系列独立显卡（RX 9070 XT / 9070 等，RDNA4 架构）：
    -> 运行时自动派发至原生 FP8 协作矩阵核心（Wave32 原生指令），零内存与转换开销。
  - AMD Radeon RX 7000 全系列独立显卡（RX 7900 XTX / XT / 7800 XT / 7700 XT / 7600 等，RDNA3 架构）：
    -> 运行时自动激活 FP16 WMMA 孪生缓冲转换与 kswap 偶数行转置链路，彻底解决泛绿屏与崩溃。
  - AMD 移动端 APU / 掌机芯片：
    - AMD Ryzen AI MAX+ 395 / Ryzen AI MAX 390 (Strix Halo Radeon 8060S / 8050S 40CU/32CU)
    - AMD Ryzen AI 9 HX 370 / 365 (Strix Point Radeon 890M / 880M)
    - AMD Ryzen 7 7840U / 8840U / 7840HS / 8840HS / 7940HS (Phoenix / Hawk Point Radeon 780M)

【重大架构升级：单 DLL 跨架构运行时动态派发】
  1. 【真正的单 DLL 自动兼容 (Unified Single DLL Architecture)】
     - 彻底告别分发阶段必须选择 RDNA3 专版或 RDNA4 专版的割裂现状。
     - 编译层完全消除硬编码宏分支（彻底重构 #if NR_ARCH_RDNA3），统一为一个单 DLL。
     - 整合包内同时内置 RDNA3 与 RDNA4 两套 ACO 机器码切片 (aco/rdna3/ 与 aco/rdna4/) 以及
       双架构着色器 (shaders/rdna3/ 与 shaders/rdna4/)。
  2. 【多显卡 / 混合显卡 (LUID 渲染设备精准绑定)】
     - 无论用户电脑是“独显+核显”（如 RX 9070 XT + Radeon 780M）、双独显还是笔记本双显卡切换，
       插件均会在 Direct3D 12/11 初始化时读取游戏正在渲染的 Adapter LUID，并与底层 Vulkan 物理
       设备严格一一对应匹配。
     - 游戏在 9070 XT 上渲染，插件自动以 RDNA4 原生 FP8 模式运行；如果游戏在 780M 上渲染，插件
       自动以 RDNA3 FP16 模式运行，绝不会因系统内存在多张显卡而错配架构或引发崩溃！
  3. 【绿色屏幕彻底根治】
     - 完整修复 RDNA3 WMMA 偶数行寄存器跨通道转置 (kswap) 及原地运动向量采样描述符布局，
       输出画面色彩平衡，无任何偏绿、泛绿与数值溢出异常。
  4. 【ACO 硬件级着色器编译】
     - 默认强制开启 [ACO Mode = 1]，预置全套 ACO 机器码切片 (*.nrp)，杜绝 Windows 原生驱动崩溃。
  5. 【CC_SMALL_FP8 极速模型默认预置】
     - 默认激活全新 SMALL FP8 矩阵量化网络，模型大小从 141MB 精简至 108MB，大幅提升降噪帧率。

-------------------------------------------------------------------------------
【安装方法（两种方式任选）】
-------------------------------------------------------------------------------
方式 A（最简单：一键批处理安装）：
  1. 将解压出来的所有文件保持在同一文件夹；
  2. 把游戏的主程序（例如 Cyberpunk2077.exe 或 SB-Win64-Shipping.exe）直接【拖动】到【一键安装到游戏.bat】上；
     （如果是虚幻引擎如《剑星》，即使拖动根目录的 SB.exe，批处理也会智能识别并自动安装到 SB\Binaries\Win64\ 真实渲染目录！）
  3. 批处理会自动备份原先的 dxgi.dll 并完成一键部署！

方式 B（手动复制）：
  1. 直接将本文件夹内的以下所有文件/文件夹全部复制到游戏【真正运行的 64 位主程序】所在目录：
     ★ 特别注意：虚幻引擎游戏（如剑星、黑神话等）请复制到 <游戏目录>\SB\Binaries\Win64\（与 *Shipping.exe 同级）！
     - dxgi.dll
     - OptiScaler.dll
     - dlssnr_core.dll
     - nvngx.dll_dlssnr.dll
     - nvngx_dlssnr.dll
     - OptiScaler.ini
     - dlssnr-amd.ini
     - OptiScaler\ (文件夹)
     - dlssnr-amd\ (文件夹)

-------------------------------------------------------------------------------
【游戏内操作与快捷键】
-------------------------------------------------------------------------------
  - [Insert 键]   : 打开 OptiScaler 菜单（调节超分辨率 FSR/XeSS、帧生成 OptiFG 等）。
  - [End 键]      : 打开 DLSS-NR Control 原生菜单（调节降噪强度、锐化、曝光曲线等）。
  - [Ctrl + F11]  : 热切换网络模型（在 CC_SMALL_FP8 极速网络 与 CC_SILVER_AARDWOLD 之间切换）。
  - [Ctrl + F10]  : 开启/关闭图像预处理。
  - [F5 键]       : 自动保存一组无降噪与有降噪的对比原图到游戏目录的 "NR Screenshots" 文件夹。

-------------------------------------------------------------------------------
【常见问题与排查】
-------------------------------------------------------------------------------
Q1: 游戏启动闪退或报错？
A1: 检查游戏目录是否生成 dlssnr-amd.log 与 OptiScaler.log。如果游戏有反作弊（如多人网游），
    请勿在在线模式下使用注入模组。
Q2: 感觉降噪画面有点过锐或噪点风格不习惯？
A2: 按 [End] 呼出菜单，在 [Network] 或 [Preprocess] 调整 TransferStrength 或关闭 Style Grade。
Q3: 怎样彻底卸载？
A3: 运行【一键卸载.bat】，或者直接删除游戏目录下的 dxgi.dll, OptiScaler.dll, dlssnr_core.dll,
    OptiScaler.ini, dlssnr-amd.ini, OptiScaler 文件夹与 dlssnr-amd 文件夹即可。

===============================================================================
'''

# 5. 一键收集日志.bat (Pure 7-bit ASCII calling install.ps1 -Route logs)
collect_logs_bat = r'''@echo off
setlocal
cd /d "%~dp0"
title DLSS-NR AMD Log Collector
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install.ps1" -Route logs %*
pause
'''

(PKG_DIR / "一键安装到游戏.bat").write_text(install_bat.replace("\n", "\r\n"), encoding="ascii")
(PKG_DIR / "install_game.ps1").write_text(install_game_ps1.replace("__VERSION__", VERSION).replace("\n", "\r\n"), encoding="utf-8-sig")
(PKG_DIR / "一键卸载.bat").write_text(uninstall_bat.replace("\n", "\r\n"), encoding="ascii")
(PKG_DIR / "uninstall_game.ps1").write_text(uninstall_game_ps1.replace("\n", "\r\n"), encoding="utf-8-sig")
(PKG_DIR / "一键收集日志.bat").write_text(collect_logs_bat.replace("\n", "\r\n"), encoding="ascii")
(PKG_DIR / "README_使用与实测指南.txt").write_text(readme_txt.replace("__VERSION__", VERSION).replace("\n", "\r\n"), encoding="utf-8")

# Also copy install.bat and install.ps1 for standard upstream compatibility
shutil.copy2(REPO_ROOT / "windows/package/install.bat", PKG_DIR / "install.bat")
shutil.copy2(REPO_ROOT / "windows/package/install.ps1", PKG_DIR / "install.ps1")

print("[8/8] Packaging into zip file...")
DST_ZIP = Path(f"/mnt/c/Users/Kenny/Downloads/RDNA3&3.5-DLSSNR-GameTest-v{VERSION}.zip")
if DST_ZIP.exists():
    DST_ZIP.unlink()

with zipfile.ZipFile(DST_ZIP, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, dirs, files in os.walk(PKG_DIR):
        for f in files:
            full = Path(root) / f
            rel = full.relative_to(PKG_DIR)
            z.write(full, rel)

size_mb = DST_ZIP.stat().st_size / (1024 * 1024)
print(f"SUCCESS! Created {DST_ZIP} ({size_mb:.2f} MB)")
