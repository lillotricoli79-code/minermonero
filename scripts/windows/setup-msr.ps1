$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"
$destinationDir = Join-Path $engineDir "msr"
$archive = Join-Path $env:TEMP "pawnio-modules-0.2.11.zip"
$extractDir = Join-Path $env:TEMP "pawnio-modules-0.2.11"

$uri = "https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip"
$expectedSha256 = "43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4"

Write-Host "Downloading the signed PawnIO Intel MSR module..."
Invoke-WebRequest -Uri $uri -OutFile $archive
$actualSha256 = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    Remove-Item $archive -Force -ErrorAction SilentlyContinue
    throw "PawnIO module archive SHA-256 mismatch. The file was not installed."
}

if (Test-Path $extractDir) {
    Remove-Item $extractDir -Recurse -Force
}
New-Item -ItemType Directory -Path $extractDir | Out-Null
Expand-Archive -Path $archive -DestinationPath $extractDir -Force

$module = Get-ChildItem -Path $extractDir -Filter "IntelMSR.bin" -File -Recurse |
    Select-Object -First 1
if (-not $module) {
    throw "IntelMSR.bin was not found in the verified PawnIO module archive."
}

New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
Copy-Item -Path $module.FullName -Destination (Join-Path $destinationDir "IntelMSR.bin") -Force

$library = Join-Path $env:ProgramFiles "PawnIO\PawnIOLib.dll"
Write-Host "Verified IntelMSR module installed at: $destinationDir\IntelMSR.bin"
if (Test-Path $library) {
    Write-Host "PawnIOLib.dll detected: $library"
}
else {
    Write-Warning "PawnIO driver/library not detected. Install the official signed PawnIO driver from https://pawnio.eu/ before enabling native MSR."
}

Write-Host "Rebuild the native miner so CMake copies IntelMSR.bin beside unique-rx.exe."
