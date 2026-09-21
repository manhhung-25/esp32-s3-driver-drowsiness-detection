param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $ProjectRoot "build"
}
. "$PSScriptRoot\idf_env.ps1"

Push-Location $ProjectRoot
try {
    idf.py -B $BuildDir -p $Port flash monitor
} finally {
    Pop-Location
}
