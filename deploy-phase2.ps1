param(
    [Parameter(Mandatory = $true)]
    [string]$Address,
    [int]$Port = 5000
)

$ErrorActionPreference = "Stop"

$dist = Join-Path $PSScriptRoot "phase2\dist_engine"
if (-not (Test-Path $dist)) {
    throw "dist_engine not found. Run .\build-phase2.ps1 first."
}

Write-Host "Connecting to Switch FTP at $Address`:$Port..." -ForegroundColor Cyan

function Upload-FtpFile {
    param([string]$LocalPath, [string]$FtpRelativePath)
    $uri = "ftp://$Address`:$Port/$FtpRelativePath"
    Write-Host "  -> Uploading $FtpRelativePath..."
    $ftp = [System.Net.FtpWebRequest]::Create($uri)
    $ftp.Method = [System.Net.WebRequestMethods+Ftp]::UploadFile
    $ftp.UseBinary = $true
    $ftp.KeepAlive = $false
    $bytes = [System.IO.File]::ReadAllBytes($LocalPath)
    $ftp.ContentLength = $bytes.Length
    $stream = $ftp.GetRequestStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Close()
    $response = $ftp.GetResponse()
    $response.Close()
}

function Create-FtpDirectory {
    param([string]$FtpRelativePath)
    $uri = "ftp://$Address`:$Port/$FtpRelativePath"
    try {
        $ftp = [System.Net.FtpWebRequest]::Create($uri)
        $ftp.Method = [System.Net.WebRequestMethods+Ftp]::MakeDirectory
        $ftp.KeepAlive = $false
        $response = $ftp.GetResponse()
        $response.Close()
    } catch {}
}

Create-FtpDirectory "atmosphere"
Create-FtpDirectory "atmosphere/contents"
Create-FtpDirectory "atmosphere/contents/4200000000535755"
Create-FtpDirectory "switch"
Create-FtpDirectory "switch/.overlays"

Upload-FtpFile (Join-Path $dist "atmosphere\contents\4200000000535755\exefs.nsp") "atmosphere/contents/4200000000535755/exefs.nsp"
Upload-FtpFile (Join-Path $dist "atmosphere\contents\4200000000535755\toolbox.json") "atmosphere/contents/4200000000535755/toolbox.json"
Upload-FtpFile (Join-Path $dist "switch\.overlays\Switube.ovl") "switch/.overlays/Switube.ovl"

Write-Host ""
Write-Host "Switube Phase 2 deployed to Switch successfully!" -ForegroundColor Green
Write-Host "Open the Tesla/Ultrahand menu (L + D-Pad Down + R3) in any game to launch Switube!" -ForegroundColor Green
