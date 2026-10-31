# verify.ps1 <keyA> <keyB>: exit 0 when both registry keys (reg.exe form, e.g. HKLM\SOFTWARE\...) hold the same
# subkeys, value names, types and data, recursively; prints the differences otherwise.
param([string]$A, [string]$B)
function Dump([string]$k) {
    $root = 'Registry::' + $k
    if (-not (Test-Path -LiteralPath $root)) { return $null }
    $out = New-Object System.Collections.Generic.List[string]
    $keys = @(Get-Item -LiteralPath $root) + @(Get-ChildItem -LiteralPath $root -Recurse)
    foreach ($x in $keys) {
        $rel = $x.Name.Substring($x.Name.IndexOf('\') + 1)
        $base = (Get-Item -LiteralPath $root).Name
        $rel = $x.Name.Substring($base.Length)
        $out.Add("K $rel")
        foreach ($v in $x.GetValueNames()) {
            $d = $x.GetValue($v, $null, 'DoNotExpandEnvironmentNames')
            if ($d -is [array]) { $d = ($d | % { "$_" }) -join ',' }
            $out.Add("V $rel|$v|$($x.GetValueKind($v))|$d")
        }
    }
    return ($out | Sort-Object)
}
$da = Dump $A; $db = Dump $B
if ($null -eq $da) { Write-Output "missing: $A"; exit 2 }
if ($null -eq $db) { Write-Output "missing: $B"; exit 2 }
$diff = Compare-Object $da $db
if ($diff) { $diff | % { Write-Output "$($_.SideIndicator) $($_.InputObject)" }; exit 1 }
Write-Output "same: $A == $B ($($da.Count) lines)"
exit 0
