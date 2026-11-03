# Generate PSDB + SODB + net-carriers.bin from a release bundle.
#   scripts\generate.ps1 -AcoRoot <release>\dlssnr-amd\aco -ReleaseRoot <release>\dlssnr-amd -Out C:\out
# Toolchain defaults come from the environment / build.cmd conventions:
#   ASD_SDK_DIR, ASD_PLUGIN_DIR, ASD_DXC. Or pass -CompilerDir / -Plugin / -Dxc.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$AcoRoot,
    [string]$ReleaseRoot = "",
    [string]$CompilerDir = "",
    [string]$Plugin = "",
    [string]$Dxc = "",
    [string]$Shaders = "",
    [string]$Family = "gfx1201",
    [string]$AppExe = "",
    [string]$AppName = "",
    [string]$AppVersion = "",
    [string]$AppEngine = "",
    [string]$AppEngineVersion = "",
    [string]$GeneratorDir = ""
)
$ErrorActionPreference = "Stop"
if (-not $GeneratorDir) { $GeneratorDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$exe = Join-Path $GeneratorDir "build\bin\asd_psdb_gen.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "build the generator first (scripts\build.cmd): $exe" }
if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $exe) "D3D12Core.dll"))) {
    throw "D3D12Core.dll is missing next to $exe (Agility 1.619.5, app-local runtime)"
}
if (-not $ReleaseRoot) { $ReleaseRoot = Split-Path $AcoRoot -Parent }

$Out = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$common = @("--root", $GeneratorDir, "--family", $Family)
if ($CompilerDir) { $common += @("--compiler-dir", $CompilerDir) }
if ($Plugin) { $common += @("--plugin", $Plugin) }
if ($Dxc) { $common += @("--dxc", $Dxc) }
if ($Shaders) { $common += @("--shaders", $Shaders) }
if ($AppExe) { $common += @("--app-exe", $AppExe) }
if ($AppName) { $common += @("--app-name", $AppName) }
if ($AppVersion) { $common += @("--app-version", $AppVersion) }
if ($AppEngine) { $common += @("--app-engine", $AppEngine) }
if ($AppEngineVersion) { $common += @("--app-engine-version", $AppEngineVersion) }

function Invoke-Gen([string]$Stage, [string[]]$Arguments) {
    Write-Host "[$Stage] asd_psdb_gen $($Arguments -join ' ')"
    & $exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Stage failed: exit $LASTEXITCODE" }
}

Invoke-Gen "manifest" ($common + @("--out", $Out, "--m2-manifest",
    "--aco-root", $AcoRoot, "--release-root", $ReleaseRoot))
$canaryDir = Join-Path $Out "canary"
New-Item -ItemType Directory -Force -Path $canaryDir | Out-Null
Invoke-Gen "canary" ($common + @("--out", $canaryDir, "--canary-seed"))
Invoke-Gen "databases" ($common + @("--out", $Out, "--m5-build",
    "--manifest", (Join-Path $Out "manifest.tsv"),
    "--aco-root", $AcoRoot, "--release-root", $ReleaseRoot,
    "--canary-sodb", (Join-Path $canaryDir "canary.sodb"),
    "--canary-psdb", (Join-Path $canaryDir "canary.psdb")))
Invoke-Gen "verify" ($common + @("--out", (Join-Path $Out "verify"), "--verify",
    "--manifest", (Join-Path $Out "manifest.tsv"),
    "--sodb", (Join-Path $Out "carrier.sodb"),
    "--psdb", (Join-Path $Out "public-copy.psdb"),
    "--carriers", (Join-Path $Out "net-carriers.bin")))

function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Dispose(); $sha.Dispose() }
    return ([System.BitConverter]::ToString($bytes)).Replace("-", "").ToLowerInvariant()
}
Get-ChildItem -LiteralPath $Out -File | ForEach-Object {
    "{0}  {1}" -f (Get-Sha256Hex $_.FullName), $_.Name
}
Write-Host "done: $Out"
