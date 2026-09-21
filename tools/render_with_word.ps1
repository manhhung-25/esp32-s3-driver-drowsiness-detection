param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$StatusPath
)

$ErrorActionPreference = 'Stop'
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $document = $word.Documents.Open($InputPath, $false, $true, $false)
    $document.ExportAsFixedFormat($OutputPath, 17)
    $document.Close(0)
    $word.Quit()
    'ok' | Set-Content -LiteralPath $StatusPath -NoNewline
} catch {
    $_ | Out-String | Set-Content -LiteralPath $StatusPath
    exit 1
}
