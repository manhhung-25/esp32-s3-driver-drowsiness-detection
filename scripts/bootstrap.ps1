param(
    [string]$IdfPath = "",
    [string]$BuildDir = "",
    [string]$Port = "",
    [switch]$NoMonitor,
    [switch]$WithPythonTools
)

$ErrorActionPreference = "Stop"
$IdfVersion = "v5.3.5"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($IdfPath)) {
    $IdfPath = Join-Path $env:LOCALAPPDATA "Espressif\frameworks\esp-idf-$($IdfVersion.TrimStart('v'))"
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $ProjectRoot "build"
}

function Assert-LastExitCode([string]$Step) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE."
    }
}

$exportScript = Join-Path $IdfPath "export.ps1"
if (-not (Test-Path -LiteralPath $exportScript)) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "Git is required for the first setup. Install Git, then run this script again."
    }

    $idfParent = Split-Path -Parent $IdfPath
    New-Item -ItemType Directory -Force -Path $idfParent | Out-Null
    Write-Host "Installing ESP-IDF $IdfVersion into $IdfPath ..."
    & git clone --branch $IdfVersion --depth 1 --recursive --shallow-submodules `
        https://github.com/espressif/esp-idf.git $IdfPath
    Assert-LastExitCode "ESP-IDF clone"

    & (Join-Path $IdfPath "install.ps1") esp32s3
    Assert-LastExitCode "ESP-IDF tool installation"
}

. $exportScript

if ($WithPythonTools) {
    $venvPath = Join-Path $ProjectRoot ".venv"
    if (-not (Test-Path -LiteralPath (Join-Path $venvPath "Scripts\python.exe"))) {
        python -m venv $venvPath
        Assert-LastExitCode "Python virtual environment creation"
    }
    $venvPython = Join-Path $venvPath "Scripts\python.exe"
    & $venvPython -m pip install --upgrade pip
    Assert-LastExitCode "pip upgrade"
    & $venvPython -m pip install -r (Join-Path $ProjectRoot "requirements.txt")
    Assert-LastExitCode "Python tool dependency installation"
}

Push-Location $ProjectRoot
try {
    Write-Host "Building firmware; managed components will be restored from dependencies.lock ..."
    idf.py -B $BuildDir reconfigure
    Assert-LastExitCode "ESP-IDF reconfigure"
    idf.py -B $BuildDir build
    Assert-LastExitCode "ESP-IDF build"

    if (-not [string]::IsNullOrWhiteSpace($Port)) {
        idf.py -B $BuildDir -p $Port flash
        Assert-LastExitCode "ESP-IDF flash"
        if (-not $NoMonitor) {
            idf.py -B $BuildDir -p $Port monitor
            Assert-LastExitCode "ESP-IDF monitor"
        }
    } else {
        Write-Host "Build complete. To flash, rerun with -Port COMx (for example: -Port COM7)."
    }
} finally {
    Pop-Location
}
