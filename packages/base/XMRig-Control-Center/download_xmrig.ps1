$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Api = 'https://api.github.com/repos/xmrig/xmrig/releases/latest'
$Headers = @{ 'User-Agent' = 'XMRig-Control-Center-Setup' }

function Test-WindowsX64Pe {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { return $false } # MZ
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt ($stream.Length - 6)) { return $false }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { return $false } # PE\0\0
        return ($reader.ReadUInt16() -eq 0x8664) # IMAGE_FILE_MACHINE_AMD64
    }
    finally {
        if ($reader) { $reader.Dispose() } else { $stream.Dispose() }
    }
}

# Select only the official Windows x64 release archive.
$Release = Invoke-RestMethod -Uri $Api -Headers $Headers
$Asset = $Release.assets |
    Where-Object {
        $_.name -match '(?i)(win64|windows.*(x64|x86_64|amd64)).*\.zip$'
    } |
    Select-Object -First 1
if (-not $Asset) {
    throw "Nessun archivio Windows x64 trovato nella release $($Release.tag_name). Nessun file installato è stato modificato."
}

$Id = [guid]::NewGuid().ToString('N')
$Stage = Join-Path $Root ('.xmrig-stage-' + $Id)
$Zip = Join-Path $Stage 'xmrig-release.zip'
$TargetExe = Join-Path $Root 'xmrig.exe'
$BackupDir = Join-Path $Root 'backups'
$Tag = $Release.tag_name -replace '[^A-Za-z0-9._-]', '_'
$EngineArchive = Join-Path $Root ('xmrig-engine-' + $Tag)
$PreparedExe = Join-Path $Root ('.xmrig-prepared-' + $Id + '.exe')

New-Item -ItemType Directory -Path $Stage -Force | Out-Null
try {
    Write-Host "Download XMRig $($Release.tag_name) - Windows x64 ($($Asset.name))..."
    Invoke-WebRequest -Uri $Asset.browser_download_url -OutFile $Zip -Headers $Headers
    if (-not (Test-Path -LiteralPath $Zip) -or (Get-Item -LiteralPath $Zip).Length -lt 1024) {
        throw 'Download incompleto o archivio troppo piccolo. Il miner installato non è stato modificato.'
    }

    $Extract = Join-Path $Stage 'extracted'
    Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
    $Exe = Get-ChildItem -LiteralPath $Extract -Filter xmrig.exe -File -Recurse | Select-Object -First 1
    if (-not $Exe) { throw 'xmrig.exe non trovato nel pacchetto scaricato. Il miner installato non è stato modificato.' }
    if ($Exe.Length -lt 1MB) { throw 'xmrig.exe scaricato ha dimensione anomala. Installazione annullata.' }
    if (-not (Test-WindowsX64Pe -Path $Exe.FullName)) {
        throw 'Il file scaricato non è un eseguibile Windows x64 (PE AMD64 0x8664). Installazione annullata; il miner esistente resta intatto.'
    }

    $ExeRelative = $Exe.FullName.Substring($Extract.Length).TrimStart([char[]]@('\','/'))
    if (Test-Path -LiteralPath $EngineArchive) {
        $EngineArchive = Join-Path $Root ('xmrig-engine-' + $Tag + '-' + $Id.Substring(0,8))
    }
    Move-Item -LiteralPath $Extract -Destination $EngineArchive
    $ExeInArchive = Join-Path $EngineArchive $ExeRelative
    if (-not (Test-Path -LiteralPath $ExeInArchive) -or -not (Test-WindowsX64Pe -Path $ExeInArchive)) {
        throw 'Verifica finale del motore x64 non riuscita. Il miner installato non è stato modificato.'
    }

    Copy-Item -LiteralPath $ExeInArchive -Destination $PreparedExe
    if (-not (Test-WindowsX64Pe -Path $PreparedExe)) {
        throw 'La copia preparata non supera il controllo x64. Il miner installato non è stato modificato.'
    }

    # Keep a timestamped backup before replacing an existing executable.
    if (Test-Path -LiteralPath $TargetExe) {
        New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null
        $Backup = Join-Path $BackupDir ('xmrig.exe.' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.bak')
        Copy-Item -LiteralPath $TargetExe -Destination $Backup
        Write-Host "Backup precedente conservato: $Backup"
    }

    # Prepared file is on the same volume as the target; replacement is atomic at file level.
    Move-Item -LiteralPath $PreparedExe -Destination $TargetExe -Force
    if (-not (Test-WindowsX64Pe -Path $TargetExe)) {
        throw 'Controllo x64 post-installazione fallito. Ripristinare il backup appena creato.'
    }

    Write-Host "OK: XMRig $($Release.tag_name) installato come Windows x64 (AMD64)."
    Write-Host "Percorso: $TargetExe"
    Write-Host "Archivio completo della release conservato in: $EngineArchive"
}
finally {
    if (Test-Path -LiteralPath $PreparedExe) {
        Remove-Item -LiteralPath $PreparedExe -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $Stage) {
        Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
