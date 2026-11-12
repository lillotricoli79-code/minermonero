$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$driverPath = Join-Path $repoRoot "engine\msr\WinRing0x64.sys"
$driverCandidates = @(
    $driverPath,
    (Join-Path $repoRoot "WinRing0x64.sys"),
    (Join-Path $repoRoot "unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-mingw-ninja\WinRing0x64.sys"),
    (Join-Path $repoRoot "engine\build-mingw-make\WinRing0x64.sys"),
    (Join-Path $repoRoot "release\unique-rx-windows-x64\WinRing0x64.sys")
)

Write-Host "UNIQUE RX usa il backend MSR WinRing0 nativo, compatibile con il layout XMRig."
Write-Host "Questo script non scarica, installa, firma o modifica driver di terze parti."

$foundDriver = $driverCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $foundDriver) {
    Write-Warning "WinRing0x64.sys non trovato."
    Write-Host "Puoi inserirlo accanto a unique-rx.exe oppure in:"
    Write-Host "  $driverPath"
    Write-Host "Poi esegui scripts\windows\build.ps1."
    exit 0
}
if (-not [string]::Equals([IO.Path]::GetFullPath($foundDriver), [IO.Path]::GetFullPath($driverPath), [StringComparison]::OrdinalIgnoreCase)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $driverPath) | Out-Null
    Copy-Item -LiteralPath $foundDriver -Destination $driverPath -Force
}

$driverHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "WinRing0x64.sys trovato: $driverPath" -ForegroundColor Green
Write-Host "SHA-256: $driverHash"
Write-Host "Driver pronto per essere copiato accanto a unique-rx.exe durante la build."
