param(
    [string]$Distro = "Ubuntu-22.04",
    [ValidateSet(0, 1)]
    [int]$Cpu2 = 1
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
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

$emuCommand = "cd '$wslRoot' && emcc -O2 -ffp-contract=off -std=gnu99 -w -DFELUCCA_RDX=1 -DFELUCCA_FM6_ONLY=1 -DFELUCCA_CPU2=0 -Ibuild/gen -Ifirmware/src --no-entry -sSTANDALONE_WASM -sTOTAL_STACK=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 -o build/emu/felucca.wasm web/emu/felucca_web.c"
Invoke-WslWithRetry -Arguments @("sh", "-lc", $emuCommand) | Write-Host

$emuRoot = Join-Path $root "build\emu"
$fontRoot = Join-Path $emuRoot "fonts"
New-Item -ItemType Directory -Force -Path $fontRoot | Out-Null
$html = Get-Content -Raw -LiteralPath (Join-Path $root "web\emu\index.html")
$html = $html.Replace("@VERSION@", "Felucca RDX 0.5").Replace("@PREVIEW@", "")
Set-Content -NoNewline -Encoding utf8 -LiteralPath (Join-Path $emuRoot "index.html") -Value $html
Copy-Item -Force -LiteralPath (Join-Path $root "web\emu\worklet.js") -Destination $emuRoot
Copy-Item -Force -LiteralPath (Join-Path $root "web\emu\fonts\DotGothic16-subset.woff") -Destination $fontRoot
Copy-Item -Force -LiteralPath (Join-Path $root "web\emu\fonts\OFL.txt") -Destination $fontRoot

& node (Join-Path $root "web\emu\rdx_test.mjs") (Join-Path $emuRoot "felucca.wasm")
if ($LASTEXITCODE -ne 0) { throw "RDX emulator regression failed" }

$siteRoot = Join-Path $root "build\site"
& python (Join-Path $root "web\make_site.py") (Join-Path $root "build\felucca.fwsc") "rdx-0.5" $siteRoot
if ($LASTEXITCODE -ne 0) { throw "Local web installer generation failed" }

Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root "build\felucca.bin"), (Join-Path $root "build\felucca.fwsc") |
    Format-Table -AutoSize
$coreTag = if ($Cpu2) { "dualcore" } else { "singlecore" }
$namedFirmware = Join-Path $root "build\felucca-rdx-$coreTag.fwsc"
Copy-Item -Force -LiteralPath (Join-Path $root "build\felucca.fwsc") -Destination $namedFirmware
Write-Host "Firmware:  $root\build\felucca.fwsc"
Write-Host "Named copy: $namedFirmware"
Write-Host "Emulator:  $root\build\emu"
Write-Host "Installer: $root\build\site\webapp\installer"
