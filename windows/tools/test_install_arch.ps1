$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Join-Path $root 'windows/package/install.ps1'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$functions = @('Get-ExeBits', 'Select-Rdna2Gpu', 'Update-InstallIniLines', 'Record', 'Save-Existing', 'Put-File', 'Put-Tree', 'Remove-Installed', 'Record-Logs', 'Move-OtherAddons', 'Put-ReShade')
foreach ($node in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    if ($node.Name -in $functions) { . ([scriptblock]::Create($node.Extent.Text)) }
}
function Say([string]$text) {}
function Fail([string]$text) { throw $text }
if (-not $IsWindows) { function Unblock-File { param([Parameter(ValueFromPipeline)]$InputObject, [string]$LiteralPath) process {} } }
function Check([bool]$ok, [string]$name) { if (-not $ok) { throw $name }; Write-Output "PASS: $name" }

$text = [IO.File]::ReadAllText($source)
$start = $text.IndexOf('# ---- choose the route')
$end = $text.IndexOf('# ---- the network a game starts with')
$choose = [scriptblock]::Create($text.Substring($start, $end - $start))
$dispatch = $ast.EndBlock.Statements | Where-Object { $_.Extent.Text.StartsWith("if (`$Route -eq 'optiscaler') {") } | Select-Object -First 1
if (-not $dispatch) { throw 'Route dispatch not found' }
$install = [scriptblock]::Create($dispatch.Extent.Text)
$scratch = Join-Path $root 'artifacts/windows/install-arch-test'
if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force }
New-Item -ItemType Directory -Path $scratch | Out-Null
$here = Join-Path $scratch 'package'
$ManifestName = 'dlssnr-amd-install.txt'; $BackupName = 'dlssnr-amd-backup'
$files = @('reshade/ReShade32.dll', 'reshade/ReShade64.dll', 'reshade/dlssnr_amd.addon32', 'reshade/dlssnr_amd.addon64',
    'reshade/ReShade.ini', 'reshade/ReShadePreset.ini', 'reshade/ReShadePreset-d3d9.ini', 'reshade/reshade-shaders/Shaders/feed.fx',
    'reshade/vk-override/implicit_layer/ReShade32.json', 'reshade/vk-override/implicit_layer/ReShade64.json',
    'vulkan/vulkan-1.dll', 'vulkan/x86/vulkan-1.dll', 'dxvk/d3d9.dll', 'dxvk/dxgi.dll', 'dxvk/x86/d3d9.dll', 'dxvk/x86/dxgi.dll')
foreach ($file in $files) {
    $path = Join-Path $here $file
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $file)
}
foreach ($arch in @(32, 64)) {
    foreach ($Route in @('reshade-feeder', 'reshade-feeder-dx9', 'reshade-feeder-vulkan')) {
        $game = Join-Path $scratch "$arch-$Route"
        New-Item -ItemType Directory -Path $game | Out-Null
        $exe = Join-Path $game 'game.exe'; $exeName = 'game'
        $pe = New-Object byte[] 128
        [BitConverter]::GetBytes([int]64).CopyTo($pe, 0x3C)
        [BitConverter]::GetBytes([uint16]$(if ($arch -eq 32) { 0x14C } else { 0x8664 })).CopyTo($pe, 68)
        [IO.File]::WriteAllBytes($exe, $pe)
        . $choose
        Check ($bits -eq $arch) "$arch architecture selected"
        $manifest = Join-Path $game $ManifestName; $backup = Join-Path $game $BackupName
        Set-Content $manifest "F $ManifestName"
        [IO.File]::WriteAllText((Join-Path $game 'dxgi.dll'), 'original proxy')
        [IO.File]::WriteAllText((Join-Path $game "other.addon$arch"), 'original addon')
        New-Item -ItemType Directory -Path (Join-Path $game 'vk-override') | Out-Null
        [IO.File]::WriteAllText((Join-Path $game 'vk-override/original.txt'), 'original layer')
        . $install
        $opposite = if ($arch -eq 32) { 64 } else { 32 }
        Check ((Test-Path (Join-Path $game $addon)) -and -not (Test-Path (Join-Path $game "dlssnr_amd.addon$opposite"))) "$arch $Route installs only selected addon"
        Check (-not (Test-Path (Join-Path $game "other.addon$arch"))) "$arch conflicting addon backed up"
        if ($Route -eq 'reshade-feeder') {
            Check ([IO.File]::ReadAllText((Join-Path $game 'dxgi.dll')) -eq "reshade/ReShade$arch.dll") "$arch native ReShade proxy selected"
        } else {
            Check ((Test-Path (Join-Path $game "vk-override/implicit_layer/ReShade$arch.json")) -and -not (Test-Path (Join-Path $game "vk-override/implicit_layer/ReShade$opposite.json"))) "$arch only selected layer installed"
            Check ([IO.File]::ReadAllText((Join-Path $game 'vulkan-1.dll')) -eq "vulkan/$($binaryDir.Replace('\', '/'))vulkan-1.dll") "$arch loader selected"
            if ($Route -eq 'reshade-feeder-dx9') {
                Check ([IO.File]::ReadAllText((Join-Path $game 'd3d9.dll')) -eq "dxvk/$($binaryDir.Replace('\', '/'))d3d9.dll") "$arch DXVK selected"
            }
        }
        Remove-Installed
        Check (([IO.File]::ReadAllText((Join-Path $game 'dxgi.dll')) -eq 'original proxy') -and (Test-Path (Join-Path $game "other.addon$arch")) -and (Test-Path (Join-Path $game 'vk-override/original.txt'))) "$arch $Route uninstall restores originals"
        Check (-not (Test-Path (Join-Path $game $addon))) "$arch addon removed"
    }
}
foreach ($Route in @('optiscaler', 'standalone')) {
    $exe = Join-Path $scratch '32-reshade-feeder/game.exe'
    $rejected = $false
    try { . $choose } catch { $rejected = $_.Exception.Message -like '32-bit games use the reshade-feeder*' }
    Check $rejected "x86 $Route rejected"
}
$Route = 'reshade-feeder'; $exe = Join-Path $scratch 'empty.exe'
[IO.File]::WriteAllBytes($exe, @())
$rejected = $false
try { . $choose } catch { $rejected = $_.Exception.Message -like "Cannot determine the game's architecture:*" }
Check $rejected 'unknown architecture rejected'

# RDNA2 GPU detection (discrete GPUs prioritized over integrated GPUs)
function Make-Gpu([string]$name, [string]$devHex) {
    [pscustomobject]@{ Name = $name; PNPDeviceID = "PCI\VEN_1002&DEV_${devHex}&SUBSYS_00000000&REV_C1\6&1234&0&0008" }
}
Check ((Select-Rdna2Gpu @(Make-Gpu 'AMD Radeon RX 6750 GRE 12GB' '73DF')) -eq 'AMD Radeon RX 6750 GRE 12GB') 'detects standalone RDNA2 dGPU'
Check ((Select-Rdna2Gpu @((Make-Gpu 'AMD Radeon 780M Graphics' '1900'), (Make-Gpu 'AMD Radeon RX 6900 XT' '73BF'))) -eq 'AMD Radeon RX 6900 XT') '780M + RX 6900 XT picks RDNA2 dGPU'
Check ($null -eq (Select-Rdna2Gpu @((Make-Gpu 'AMD Radeon(TM) Graphics' '164E'), (Make-Gpu 'AMD Radeon RX 9070 XT' '7550')))) 'RDNA2 iGPU + RX 9070 XT keeps default'
Check ($null -eq (Select-Rdna2Gpu @((Make-Gpu 'AMD Radeon(TM) Graphics' '164E'), (Make-Gpu 'AMD Radeon RX 7900 XTX' '744C')))) 'RDNA2 iGPU + RX 7900 XTX keeps default'
Check ($null -eq (Select-Rdna2Gpu @((Make-Gpu 'AMD Radeon RX 9070 XT' '7550'), (Make-Gpu 'AMD Radeon RX 6900 XT' '73BF')))) 'RX 9070 XT + RX 6900 XT dGPU keeps default'
Check ($null -eq (Select-Rdna2Gpu @(Make-Gpu 'AMD Radeon 780M Graphics' '1900'))) 'pure 780M RDNA3 iGPU keeps default'
Check ((Select-Rdna2Gpu @(Make-Gpu 'AMD Radeon 680M Graphics' '1681')) -eq 'AMD Radeon 680M Graphics') 'pure 680M RDNA2 iGPU detected'

# RDNA2 INI patching against generated template INIs
$py = Get-Command python3, python -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $py) { throw 'python3 or python not found for make_ini.py' }
$iniDir = Join-Path $scratch 'ini'
& $py.Source (Join-Path $root 'windows/package/make_ini.py') (Join-Path $root 'windows/src/pe/nr_pe_config.cpp') $iniDir | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'make_ini.py failed' }

$optLines = [IO.File]::ReadAllLines((Join-Path $iniDir 'dlssnr-amd-optiscaler-renodx.ini'), [Text.Encoding]::UTF8)
$optOn = (Update-InstallIniLines $optLines 'optiscaler' 'CC_SMALL_FP8' 'on' 'on' $false) -join "`n"
Check ($optOn -match '(?m)^Model = CC_SMALL_FP8$' -and $optOn -match '(?m)^Rdna2Int8 = 1$' -and $optOn -match '(?m)^NRHookPoint = 1$') 'optiscaler RDNA2 preset sets Model, Rdna2Int8=1, NRHookPoint=1'

$optOff = (Update-InstallIniLines $optLines 'optiscaler' 'CC_SMALL_FP8' 'on' 'off' $false) -join "`n"
Check ($optOff -match '(?m)^Model = CC_SMALL_FP8$' -and $optOff -match '(?m)^Rdna2Int8 = 0$' -and $optOff -match '(?m)^NRHookPoint = 2$') 'optiscaler non-RDNA2 keeps Rdna2Int8=0 and NRHookPoint=2'

$addonLines = [IO.File]::ReadAllLines((Join-Path $iniDir 'dlssnr-amd-addon.ini'), [Text.Encoding]::UTF8)
$addonOn = (Update-InstallIniLines $addonLines 'reshade-feeder' 'CC_SMALL_FP8' 'on' 'on' $false) -join "`n"
Check ($addonOn -match '(?m)^Enabled = 1$' -and $addonOn -match '(?m)^Model = CC_SMALL_FP8$' -and $addonOn -match '(?m)^Rdna2Int8 = 1$' -and $addonOn -match '(?m)^NRHookPoint = 2$') 'reshade-feeder RDNA2 preset keeps NRHookPoint=2'

Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
Write-Output 'installer architecture tests: 0 failures'
