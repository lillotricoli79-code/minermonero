$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\\..")).Path
$destinationDir = Join-Path $repoRoot "engine\\msr"
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("unique-rx-pawnio-" + [guid]::NewGuid().ToString("N"))
$archive = Join-Path $tempRoot "release_0_2_11.zip"
$extractDir = Join-Path $tempRoot "extracted"

$uri = "https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip"
$expectedArchiveSha256 = "43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4"
$expectedModuleSha256 = "d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f"

try {
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    Write-Host "Downloading signed PawnIO Modules release 0.2.11..."
    Invoke-WebRequest -Uri $uri -OutFile $archive -UseBasicParsing

    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($archiveHash -ne $expectedArchiveSha256) {
        throw "PawnIO module archive SHA-256 mismatch. Refusing to install."
    }

    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $extractDir -Force
    $module = Get-ChildItem -LiteralPath $extractDir -Filter "IntelMSR.bin" -File -Recurse |
        Select-Object -First 1
    if (-not $module) {
        throw "IntelMSR.bin was not found in the verified PawnIO module archive."
    }

    $moduleHash = (Get-FileHash -LiteralPath $module.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($moduleHash -ne $expectedModuleSha256) {
        throw "IntelMSR.bin SHA-256 mismatch. Refusing to install."
    }

    New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
    Copy-Item -LiteralPath $module.FullName -Destination (Join-Path $destinationDir "IntelMSR.bin") -Force

    Write-Host "Verified IntelMSR.bin installed in $destinationDir"
    $pawnioKey = "HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO"
    $pawnioInstall = Get-ItemProperty -LiteralPath $pawnioKey -ErrorAction SilentlyContinue
    $pawnioLibrary = Join-Path $env:ProgramFiles "PawnIO\\PawnIOLib.dll"
    if ($pawnioInstall -or (Test-Path -LiteralPath $pawnioLibrary)) {
        Write-Host "PawnIO installation detected."
    }
    else {
        Write-Warning "Install the official signed PawnIO driver from https://pawnio.eu/ before enabling native MSR."
    }

    Write-Host "Rebuild the native miner. CMake will copy the verified module beside unique-rx.exe."
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
