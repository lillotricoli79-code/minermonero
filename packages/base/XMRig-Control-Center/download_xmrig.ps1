$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Api = 'https://api.github.com/repos/xmrig/xmrig/releases/latest'
$Headers = @{ 'User-Agent' = 'XMRig-Control-Center-Setup' }

# Stage everything separately. Never delete or overwrite the currently installed
# engine until the new archive has been downloaded and validated.
$Release = Invoke-RestMethod -Uri $Api -Headers $Headers
$Asset = $Release.assets |
    Where-Object { $_.name -match 'win64.*\.zip$' -or $_.name -match 'windows.*64.*\.zip$' } |
    Select-Object -First 1
if (-not $Asset) {
    throw "Nessun archivio Windows x64 trovato nella release $($Release.tag_name). Scaricare manualmente da https://github.com/xmrig/xmrig/releases"
}

$Id = [guid]::NewGuid().ToString('N')
$Stage = Join-Path $Root ('.xmrig-stage-' + $Id)
$Zip = Join-Path $Stage 'xmrig-release.zip'
$NewExe = Join-Path $Stage 'xmrig.exe.new'
$TargetExe = Join-Path $Root 'xmrig.exe'
$BackupDir = Join-Path $Root 'backups'
$EngineArchive = Join-Path $Root ('xmrig-engine-' + ($Release.tag_name -replace '[^A-Za-z0-9._-]', '_'))

New-Item -ItemType Directory -Path $Stage -Force | Out-Null
try {
    Write-Host "Download XMRig $($Release.tag_name)..."
    Invoke-WebRequest -Uri $Asset.browser_download_url -OutFile $Zip -Headers $Headers
    if (-not (Test-Path -LiteralPath $Zip) -or (Get-Item -LiteralPath $Zip).Length -lt 1024) {
        throw 'Download incompleto o archivio troppo piccolo.'
    }

    $Extract = Join-Path $Stage 'extracted'
    Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
    $Exe = Get-ChildItem -LiteralPath $Extract -Filter xmrig.exe -File -Recurse | Select-Object -First 1
    if (-not $Exe) { throw 'xmrig.exe non trovato nell’archivio scaricato.' }

    # Preserve the complete extracted release for diagnostics/rollback.
    if (Test-Path -LiteralPath $EngineArchive) {
        $EngineArchive = Join-Path $Root ('xmrig-engine-' + ($Release.tag_name -replace '[^A-Za-z0-9._-]', '_') + '-' + $Id.Substring(0,8))
    }
    Move-Item -LiteralPath $Extract -Destination $EngineArchive

    Copy-Item -LiteralPath (Join-Path $EngineArchive (Resolve-Path -LiteralPath $Exe.FullName -Relative).TrimStart('.','\','/')) -Destination $NewExe -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $NewExe)) {
        $ExeFromArchive = Get-ChildItem -LiteralPath $EngineArchive -Filter xmrig.exe -File -Recurse | Select-Object -First 1
        if (-not $ExeFromArchive) { throw 'Il file xmrig.exe non è più presente nello staging.' }
        Copy-Item -LiteralPath $ExeFromArchive.FullName -Destination $NewExe
    }

    if ((Get-Item -LiteralPath $NewExe).Length -lt 1MB) {
        throw 'xmrig.exe scaricato ha dimensione anomala; installazione annullata.'
    }

    if (Test-Path -LiteralPath $TargetExe) {
        New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null
        $Backup = Join-Path $BackupDir ('xmrig.exe.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
        Copy-Item -LiteralPath $TargetExe -Destination $Backup
        Write-Host "Backup precedente: $Backup"
    }

    # Atomic replacement on the same volume: prepare beside the target, then move.
    $Replace = Join-Path $Root ('.xmrig-replace-' + $Id + '.exe')
    Move-Item -LiteralPath $NewExe -Destination $Replace
    Move-Item -LiteralPath $Replace -Destination $TargetExe -Force
    Write-Host "XMRig $($Release.tag_name) installato: $TargetExe"
    Write-Host "Archivio sorgente conservato in: $EngineArchive"
}
catch {
    Write-Error $_
    throw
}
finally {
    if (Test-Path -LiteralPath $Stage) {
        Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
