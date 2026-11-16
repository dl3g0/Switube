$root = $PSScriptRoot
$staging = Join-Path $root "staging_sd"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path "$staging\switch\Switube" | Out-Null
New-Item -ItemType Directory -Force -Path "$staging\switch\.overlays" | Out-Null
New-Item -ItemType Directory -Force -Path "$staging\atmosphere\contents\4200000000535755" | Out-Null
New-Item -ItemType Directory -Force -Path "$staging\config\Switube\downloads" | Out-Null

$ovlPath = Join-Path $root "phase2\dist_engine\switch\.overlays\Switube.ovl"
if (-not (Test-Path $ovlPath)) { $ovlPath = "C:\Users\diego\Desktop\Switube\phase2\dist_engine\switch\.overlays\Switube.ovl" }
$exefsPath = Join-Path $root "phase2\dist_engine\atmosphere\contents\4200000000535755\exefs.nsp"
if (-not (Test-Path $exefsPath)) { $exefsPath = "C:\Users\diego\Desktop\Switube\phase2\dist_engine\atmosphere\contents\4200000000535755\exefs.nsp" }

Copy-Item (Join-Path $root "build_app\Switube.nro") "$staging\switch\Switube\Switube.nro"
Copy-Item $ovlPath "$staging\switch\.overlays\Switube.ovl"
Copy-Item $exefsPath "$staging\atmosphere\contents\4200000000535755\exefs.nsp"
Copy-Item (Join-Path $root "phase2\sysmodule\toolbox.json") "$staging\atmosphere\contents\4200000000535755\toolbox.json"

$zip1 = Join-Path $root "Switube-Complete-SD.zip"
if (Test-Path $zip1) { Remove-Item -Force $zip1 }

Compress-Archive -Path "$staging\*" -DestinationPath $zip1 -CompressionLevel Optimal
Remove-Item -Recurse -Force $staging

$mirrorDir = "C:\Users\diego\Desktop\Switube"
if ((Test-Path $mirrorDir) -and ($root -ne $mirrorDir)) {
    Copy-Item (Join-Path $root "build_app\Switube.nro") (Join-Path $mirrorDir "build_app\Switube.nro") -Force
    Copy-Item $zip1 (Join-Path $mirrorDir "Switube-Complete-SD.zip") -Force
}

Write-Host "SUCCESS: Created $zip1"
