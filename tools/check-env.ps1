# Meter Gomoku - toolchain self-check (Windows).
# Probes the components needed to rebuild from source; runtime needs nothing.
# Usage:  powershell -ExecutionPolicy Bypass -File check-env.ps1

$ErrorActionPreference = 'SilentlyContinue'

# Probe a command on PATH; report name + version + PASS/FAIL.
function Test-Tool([string]$Name, [string]$Command, [string]$VersionArg) {
    $cmd = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $cmd) {
        Write-Output ("{0,-14} FAIL  (not on PATH)" -f $Name)
        return
    }
    $ver = & $cmd.Source $VersionArg 2>&1 | Select-Object -First 1
    Write-Output ("{0,-14} PASS  {1}" -f $Name, $ver)
}

Write-Output "=== Meter Gomoku toolchain check ==="
Write-Output "--- Build tools ---"
Test-Tool "git"       "git"       "--version"
Test-Tool "cmake"     "cmake"     "--version"
Test-Tool "cl (MSVC)" "cl"        ""
Test-Tool "node"      "node"      "--version"
Test-Tool "python"    "python"    "--version"

Write-Output "--- OpenCV ---"
$ocv = Get-ChildItem Env:OpenCV_DIR -ErrorAction SilentlyContinue
if ($ocv) {
    Write-Output ("OpenCV_DIR      PASS  {0}" -f $ocv.Value)
} else {
    Write-Output "OpenCV_DIR      FAIL  (set OpenCV_DIR to the built OpenCV root)"
}

Write-Output "--- Runtime sanity (released app) ---"
$app = Join-Path $PSScriptRoot "..\Desktop GomokuTrainer.exe"
if (Test-Path -LiteralPath $app) {
    Write-Output ("app exe         PASS  {0}" -f (Split-Path -Leaf $app))
} else {
    Write-Output "app exe         MISSING (release build not staged here)"
}
