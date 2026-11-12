$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$driverPath = Join-Path $repoRoot "engine\msr\WinRing0x64.sys"

Write-Host "UNIQUE RX usa il backend MSR WinRing0 nativo, compatibile con il layout XMRig."
Write-Host "Questo script non scarica, installa, firma o modifica driver di terze parti."

if (-not (Test-Path -LiteralPath $driverPath -PathType Leaf)) {
    Write-Warning "WinRing0x64.sys non trovato."
    Write-Host "Inserisci il driver WinRing0x64.sys fornito da te in:"
    Write-Host "  $driverPath"
    Write-Host "Poi esegui scripts\windows\build.ps1 per copiarlo accanto a unique-rx.exe."
    exit 0
}

$driverHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "WinRing0x64.sys trovato: $driverPath" -ForegroundColor Green
Write-Host "SHA-256: $driverHash"
Write-Host "Driver pronto per essere copiato accanto a unique-rx.exe durante la build."
