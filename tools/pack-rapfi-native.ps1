# Meter Gomoku - verify the rapfi-native engine bundle.
# Cross-checks every RapfiEngine variant in ../rapfi-native against the
# SHA-256 hashes recorded in engines.json, then prints a manifest.
# Usage:  powershell -ExecutionPolicy Bypass -File pack-rapfi-native.ps1

$ErrorActionPreference = 'Stop'

$nativeDir = Join-Path $PSScriptRoot "..\rapfi-native"
$manifest  = Join-Path $nativeDir "engines.json"

if (-not (Test-Path -LiteralPath $manifest)) {
    throw "engines.json not found: $manifest"
}

# Parse the variant manifest (JSON).
$spec = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json

Write-Output "=== rapfi-native engine variants ==="
$allOk = $true
foreach ($name in $spec.variants.PSObject.Properties.Name) {
    $v = $spec.variants.$name
    $file = Join-Path $nativeDir $v.file
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Output ("{0,-14} MISSING {1}" -f $name, $v.file)
        $allOk = $false
        continue
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    $ok = $actual -eq $v.sha256
    if (-not $ok) { $allOk = $false }
    Write-Output ("{0,-14} {1}  {2}" -f $name, $(if ($ok) { "OK  " } else { "MISMATCH" }), $v.file)
}

Write-Output "---"
if ($allOk) {
    Write-Output "All variants verified against engines.json."
} else {
    Write-Output "FAIL: one or more variants missing or hash mismatch."
    exit 1
}
