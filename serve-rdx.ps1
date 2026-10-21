param(
    [ValidateSet("emulator", "installer")]
    [string]$Mode = "emulator",
    [int]$Port = 0
)

$ErrorActionPreference = "Stop"
if ($Mode -eq "emulator") {
    $folder = Join-Path $PSScriptRoot "build\emu"
    if (-not $Port) { $Port = 8790 }
    $url = "http://localhost:$Port/"
} else {
    $folder = Join-Path $PSScriptRoot "build\site"
    if (-not $Port) { $Port = 8000 }
    $url = "http://localhost:$Port/webapp/installer/"
}

if (-not (Test-Path -LiteralPath $folder)) {
    throw "RDX build output not found. Run .\build-rdx.ps1 first."
}

Write-Host "$Mode available at $url"
Write-Host "Press Ctrl+C to stop the local server."
python -m http.server $Port -d $folder
