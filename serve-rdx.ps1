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
    $normalPkg = Join-Path $PSScriptRoot "build\felucca.fwsc"
    $cleanPkg = Join-Path $PSScriptRoot "build\felucca-clean.fwsc"
    if (-not (Test-Path -LiteralPath $normalPkg) -or -not (Test-Path -LiteralPath $cleanPkg)) {
        throw "RDX firmware packages not found. Run .\build-rdx.ps1 first."
    }
    $sitePackages = @(Get-ChildItem -LiteralPath (Join-Path $folder "firmware") -Filter "felucca-*.fwsc" -ErrorAction SilentlyContinue)
    $newestSite = $sitePackages | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $newestBuild = Get-Item -LiteralPath $normalPkg, $cleanPkg | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $newestSite -or $newestBuild.LastWriteTimeUtc -gt $newestSite.LastWriteTimeUtc) {
        $versionLine = Get-Content -LiteralPath (Join-Path $PSScriptRoot "firmware\src\rdx_version.h") |
            Where-Object { $_ -match '^#define RDX_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$' } | Select-Object -First 1
        if (-not $versionLine) { throw "Cannot read RDX_VERSION" }
        $version = [regex]::Match($versionLine, '"([^"]+)"').Groups[1].Value
        & python (Join-Path $PSScriptRoot "web\make_site.py") $normalPkg "rdx-$version" $folder $cleanPkg
        if ($LASTEXITCODE -ne 0) { throw "Local installer refresh failed" }
    }
}

if (-not (Test-Path -LiteralPath $folder)) {
    throw "RDX build output not found. Run .\build-rdx.ps1 first."
}

Write-Host "$Mode available at $url"
Write-Host "Press Ctrl+C to stop the local server."
python -m http.server $Port -d $folder
