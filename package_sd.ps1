$staging = "C:\Users\diego\Desktop\Switube\staging_sd"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path "$staging\switch\Switube"
New-Item -ItemType Directory -Force -Path "$staging\switch\.overlays"
New-Item -ItemType Directory -Force -Path "$staging\atmosphere\contents\4200000000535755"
New-Item -ItemType Directory -Force -Path "$staging\config\Switube\downloads"

Copy-Item "C:\Users\diego\Desktop\Switube\build_app\Switube.nro" "$staging\switch\Switube\Switube.nro"
Copy-Item "C:\Users\diego\Desktop\Switube\phase2\dist_engine\switch\.overlays\Switube.ovl" "$staging\switch\.overlays\Switube.ovl"
Copy-Item "C:\Users\diego\Desktop\Switube\phase2\dist_engine\atmosphere\contents\4200000000535755\exefs.nsp" "$staging\atmosphere\contents\4200000000535755\exefs.nsp"
Copy-Item "C:\Users\diego\Desktop\Switube\phase2\sysmodule\toolbox.json" "$staging\atmosphere\contents\4200000000535755\toolbox.json"

$zip1 = "C:\Users\diego\Desktop\Switube\Switube-Complete-SD.zip"
if (Test-Path $zip1) { Remove-Item -Force $zip1 }

Compress-Archive -Path "$staging\*" -DestinationPath $zip1 -CompressionLevel Optimal
Remove-Item -Recurse -Force $staging

Write-Host "SUCCESS: Created $zip1"
