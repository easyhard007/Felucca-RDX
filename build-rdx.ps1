param(
    [string]$Distro = "Ubuntu-22.04",
    [ValidateSet(0, 1)]
    [int]$Cpu2 = 1
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$versionLine = Get-Content -LiteralPath (Join-Path $root "firmware\src\rdx_version.h") |
    Where-Object { $_ -match '^#define RDX_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$' } |
    Select-Object -First 1
if (-not $versionLine) { throw "Cannot read RDX_VERSION from firmware\src\rdx_version.h" }
$version = [regex]::Match($versionLine, '"([^"]+)"').Groups[1].Value
$toolchainRoot = Join-Path $root ".toolchains\jieli"
$sdkRoot = Join-Path $root ".toolchains\fw-AC79_AIoT_SDK"
$toolchain = Get-ChildItem -LiteralPath $toolchainRoot -Directory -Filter "jieli-linux-toolchains-*" |
    Sort-Object Name -Descending | Select-Object -First 1

if (-not $toolchain) { throw "JieLi toolchain not found under $toolchainRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot "cpu\wl82\tools\uboot.boot"))) {
    throw "AC79 SDK not found under $sdkRoot"
}

function Invoke-WslWithRetry {
    param([Parameter(Mandatory = $true)][string[]]$Arguments, [int]$Attempts = 3)
    for ($try = 1; $try -le $Attempts; $try++) {
        $result = & wsl.exe -d $Distro -e @Arguments
        if ($LASTEXITCODE -eq 0) { return $result }
        if ($try -lt $Attempts) { Start-Sleep -Seconds 2 }
    }
    throw "WSL command failed after $Attempts attempts: $($Arguments -join ' ')"
}

$wslRoot = (Invoke-WslWithRetry -Arguments @("wslpath", "-a", $root) | Select-Object -Last 1).Trim()
if (-not $wslRoot) { throw "Cannot map the project folder into WSL distro $Distro" }
$wslToolchain = "$wslRoot/.toolchains/jieli/$($toolchain.Name)"
$wslSdk = "$wslRoot/.toolchains/fw-AC79_AIoT_SDK"
$defines = "FELUCCA_RDX=1 FELUCCA_FM6_ONLY=1 FELUCCA_CPU2=$Cpu2"

$firmwareCommand = "cd '$wslRoot' && JIELI_TOOLCHAIN='$wslToolchain' AC79_SDK='$wslSdk' JIELI_DOCKER=0 $defines python3 tools/build.py"
Invoke-WslWithRetry -Arguments @("sh", "-lc", $firmwareCommand) | Write-Host

$emuCommand = "cd '$wslRoot' && python3 web/build_emu.py"
Invoke-WslWithRetry -Arguments @("sh", "-lc", $emuCommand) | Write-Host

$emuRoot = Join-Path $root "build\emu"

& node (Join-Path $root "web\emu\rdx_test.mjs") (Join-Path $emuRoot "felucca.wasm")
if ($LASTEXITCODE -ne 0) { throw "RDX emulator regression failed" }

$siteRoot = Join-Path $root "build\site"
& python (Join-Path $root "web\make_site.py") (Join-Path $root "build\felucca.fwsc") "rdx-$version" $siteRoot (Join-Path $root "build\felucca-clean.fwsc")
if ($LASTEXITCODE -ne 0) { throw "Local web installer generation failed" }

Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root "build\felucca.bin"), (Join-Path $root "build\felucca.fwsc"), (Join-Path $root "build\felucca-clean.fwsc") |
    Format-Table -AutoSize
$coreTag = if ($Cpu2) { "dualcore" } else { "singlecore" }
$namedFirmware = Join-Path $root "build\felucca-rdx-$coreTag.fwsc"
$namedCleanFirmware = Join-Path $root "build\felucca-rdx-$coreTag-clean.fwsc"
Copy-Item -Force -LiteralPath (Join-Path $root "build\felucca.fwsc") -Destination $namedFirmware
Copy-Item -Force -LiteralPath (Join-Path $root "build\felucca-clean.fwsc") -Destination $namedCleanFirmware
Write-Host "Firmware:  $root\build\felucca.fwsc"
Write-Host "Named copy: $namedFirmware"
Write-Host "Fresh install: $namedCleanFirmware"
Write-Host "Emulator:  $root\build\emu"
Write-Host "Installer: $root\build\site\webapp\installer"
