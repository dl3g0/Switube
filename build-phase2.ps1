param(
    [int]$Jobs = 4,
    [string]$DevkitPro = ""
)

$ErrorActionPreference = "Stop"
$engineVersion = "0.2.1"

$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($DevkitPro)) { $candidates += $DevkitPro }
if (-not [string]::IsNullOrWhiteSpace($env:DEVKITPRO) -and
    $env:DEVKITPRO -notmatch '^/opt/devkitpro/?$') {
    $candidates += $env:DEVKITPRO
}
$candidates += "C:\devkitPro"

$devkitRoot = $null
foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate "cmake\Switch.cmake")) {
        $devkitRoot = $candidate
        break
    }
}
if ($null -eq $devkitRoot) { throw "Could not find the devkitPro Switch toolchain" }

$msysBin = Join-Path $devkitRoot "msys2\usr\bin"
$cygpath = Join-Path $msysBin "cygpath.exe"
$cmake = Join-Path $msysBin "cmake.exe"
$ninja = Join-Path $msysBin "ninja.exe"
$make = Join-Path $msysBin "make.exe"

function Convert-ToMsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $converted = & $cygpath -u $Path
    if ($LASTEXITCODE -ne 0) { throw "cygpath failed for $Path" }
    return $converted.Trim()
}

$source = Convert-ToMsysPath (Join-Path $PSScriptRoot "phase2\sysmodule")
$buildWindows = Join-Path $PSScriptRoot "phase2\build_engine"
$build = Convert-ToMsysPath $buildWindows

$env:Path = "$devkitRoot\devkitA64\bin;$devkitRoot\tools\bin;$msysBin;$env:Path"
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"

Write-Host "==> Configuring Switube Phase 2 sysmodule..." -ForegroundColor Cyan
$configureArgs = @(
    "-S", $source,
    "-B", $build,
    "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.13"
)
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "Phase 2 engine configuration failed" }

Write-Host "==> Building Switube sysmodule package (NSP)..." -ForegroundColor Cyan
& $ninja "-C" $build "-j$Jobs" "switube-background-package"
if ($LASTEXITCODE -ne 0) { throw "Phase 2 engine build failed" }

Write-Host "==> Building Switube Ultrahand/Tesla overlay (.ovl)..." -ForegroundColor Cyan
$overlaySource = Convert-ToMsysPath (Join-Path $PSScriptRoot "phase2\overlay")
$overlayArgs = @(
    "-C", $overlaySource,
    "-j$Jobs",
    "TARGET=switube-overlay",
    "BUILD=build"
)
& $make @overlayArgs
if ($LASTEXITCODE -ne 0) { throw "Switube overlay build failed" }

$phase2Windows = Join-Path $PSScriptRoot "phase2"
$dist = Join-Path $phase2Windows "dist_engine"
if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}

$titleRoot = Join-Path $dist "atmosphere\contents\4200000000535755"
$overlayRoot = Join-Path $dist "switch\.overlays"
New-Item -ItemType Directory -Force -Path $titleRoot | Out-Null
New-Item -ItemType Directory -Force -Path $overlayRoot | Out-Null

Copy-Item -Force (Join-Path $buildWindows "switube-background.nsp") `
    (Join-Path $titleRoot "exefs.nsp")
Copy-Item -Force (Join-Path $phase2Windows "sysmodule\toolbox.json") `
    (Join-Path $titleRoot "toolbox.json")
Copy-Item -Force (Join-Path $phase2Windows "overlay\switube-overlay.ovl") `
    (Join-Path $overlayRoot "Switube.ovl")

$engineZip = Join-Path $phase2Windows "Switube-Phase2-engine-$engineVersion.zip"
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $engineZip -Force

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Green
Write-Host "Switube Phase 2 Background Playback Engine Built Successfully!" -ForegroundColor Green
Write-Host "Sysmodule: $titleRoot\exefs.nsp" -ForegroundColor Green
Write-Host "Overlay:   $overlayRoot\Switube.ovl" -ForegroundColor Green
Write-Host "SD Package Zip: $engineZip" -ForegroundColor Green
Write-Host "Title ID: 4200000000535755 ('SWTU')" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
