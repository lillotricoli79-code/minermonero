[CmdletBinding()]
param([string]$OutputDirectory=(Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) 'dist'))
$ErrorActionPreference='Stop'
$RepoRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$Base=Join-Path $RepoRoot 'packages\base\XMRig-Control-Center'
$AI=Join-Path $RepoRoot 'packages\aiqween\XMRig-Control-Center-AIQween-module'
if(-not(Test-Path (Join-Path $Base 'app.py'))){throw 'Sorgenti base non trovati.'}
if(-not(Test-Path (Join-Path $AI 'ai_qween\engine.py'))){throw 'Sorgenti AI Qween non trovati.'}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$stage=Join-Path ([IO.Path]::GetTempPath()) ('xmrig-control-center-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
  $bundle=Join-Path $stage 'XMRig-Control-Center'
  $baseTarget=Join-Path $bundle 'packages\base\XMRig-Control-Center'
  $aiTarget=Join-Path $bundle 'packages\aiqween\XMRig-Control-Center-AIQween-module'
  New-Item -ItemType Directory -Path $baseTarget -Force | Out-Null
  New-Item -ItemType Directory -Path $aiTarget -Force | Out-Null
  Copy-Item -Path (Join-Path $Base '*') -Destination $baseTarget -Recurse
  Copy-Item -Path (Join-Path $AI '*') -Destination $aiTarget -Recurse
  Get-ChildItem -LiteralPath $bundle -Directory -Recurse -Force |
    Where-Object {$_.Name -in @('__pycache__','.pytest_cache','build','dist')} |
    Sort-Object FullName -Descending | Remove-Item -Recurse -Force
  Get-ChildItem -LiteralPath $bundle -File -Recurse -Force |
    Where-Object {$_.Extension -in @('.pyc','.pyo')} | Remove-Item -Force
  $launcher=@('@echo off','setlocal','cd /d "%~dp0"','set "APP=packages\base\XMRig-Control-Center\app.py"','where py >nul 2>nul','if not errorlevel 1 (','  py -3 "%APP%"','  exit /b %errorlevel%',' )','where python >nul 2>nul','if not errorlevel 1 (','  python "%APP%"','  exit /b %errorlevel%',' )','echo Python 3.10 o superiore non trovato. Installare Python e riprovare.','pause','exit /b 1')
  Set-Content -LiteralPath (Join-Path $bundle 'START_CONTROL_CENTER.bat') -Value $launcher -Encoding ASCII
  $downloader=@('@echo off','setlocal','cd /d "%~dp0packages\base\XMRig-Control-Center"','powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0packages\base\XMRig-Control-Center\download_xmrig.ps1"','if errorlevel 1 pause','exit /b %errorlevel%')
  Set-Content -LiteralPath (Join-Path $bundle 'DOWNLOAD_XMRIG.bat') -Value $downloader -Encoding ASCII
  $manifest=Join-Path $bundle 'PACKAGE-MANIFEST.txt'
  $lines=@('XMRig Control Center + AI Qween source bundle',"Created UTC: $([DateTime]::UtcNow.ToString('o'))",'','Original XMRig ZIPs are not included; download the official XMRig Windows x64 release using packages\base\XMRig-Control-Center\download_xmrig.ps1.','')
  $lines += Get-ChildItem -LiteralPath $bundle -File -Recurse | ForEach-Object {
    $relative=$_.FullName.Substring($bundle.Length).TrimStart('\')
    $hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    "$hash  $relative"
  }
  Set-Content -LiteralPath $manifest -Value $lines -Encoding UTF8
  $zip=Join-Path $OutputDirectory "XMRig-Control-Center-AIQween-$stamp.zip"
  Compress-Archive -LiteralPath $bundle -DestinationPath $zip -CompressionLevel Optimal
  $hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
  Set-Content -LiteralPath ($zip+'.sha256') -Value "$hash  $(Split-Path -Leaf $zip)" -Encoding ASCII
  Write-Host "Bundle creato: $zip"
  Write-Host "SHA256: $hash"
  Write-Host 'Il bundle contiene sorgenti e strumenti di setup, non un xmrig.exe precompilato.'
} finally {
  if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue}
}
