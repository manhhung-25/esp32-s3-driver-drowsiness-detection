param(
    [string]$IdfPath = $env:IDF_PATH
)

$ErrorActionPreference = "Stop"

$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$localIdf = if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    $null
} else {
    Join-Path $env:LOCALAPPDATA "Espressif\frameworks\esp-idf-v5.3.5"
}

$candidates = @(
    $IdfPath,
    $localIdf,
    "C:\Espressif\frameworks\esp-idf-v5.3.5",
    "C:\Espressif\v5.3.5\esp-idf",
    "C:\Espressif\v5.3.5-shallow\esp-idf"
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

$IdfPath = $candidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "export.ps1") } |
    Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($IdfPath)) {
    throw "ESP-IDF export.ps1 not found. Run .\scripts\bootstrap.ps1 once, set IDF_PATH, or pass -IdfPath."
}

. (Join-Path $IdfPath "export.ps1")
