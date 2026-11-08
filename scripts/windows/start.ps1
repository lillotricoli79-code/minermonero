$ErrorActionPreference = "Stop"

# MSR access on Windows requires the launcher and miner process to be elevated.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Privilegi amministratore mancanti. Avvia start.bat e conferma la richiesta UAC."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$configPath = Join-Path $repoRoot "config.json"
$logsDir = Join-Path $repoRoot "logs"
$api = "http://127.0.0.1:8000"
if (-not (Test-Path -LiteralPath $configPath)) { throw "config.json non trovato. Copia config.example.json e configura wallet e pool." }

# Enable the native automatic CPU-specific MSR preset without touching wallet,
# pool, thread, TLS, or other user settings. A false wrmsr value explicitly
# disables MSR in the C++ engine, so the launcher changes it to true.
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if ($null -eq $config.randomx) {
    $config | Add-Member -NotePropertyName randomx -NotePropertyValue ([pscustomobject]@{})
}
$config.randomx | Add-Member -NotePropertyName wrmsr -NotePropertyValue $true -Force
$config.randomx | Add-Member -NotePropertyName rdmsr -NotePropertyValue $true -Force
if ([string]::IsNullOrWhiteSpace([string]$config.randomx.'msr-module')) {
    $config.randomx | Add-Member -NotePropertyName 'msr-module' -NotePropertyValue "msr/IntelMSR.bin" -Force
}
$configTemp = Join-Path $repoRoot ("config.json.msr-" + [guid]::NewGuid().ToString("N") + ".tmp")
try {
    $json = ConvertTo-Json -InputObject $config -Depth 100
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($configTemp, $json, $utf8NoBom)
    Move-Item -LiteralPath $configTemp -Destination $configPath -Force
    Write-Host "MSR abilitato nella configurazione: preset automatico CPU Intel."
}
finally {
    if (Test-Path -LiteralPath $configTemp) {
        Remove-Item -LiteralPath $configTemp -Force
    }
}

# The downloadable portable package contains the native miner but not the
# Python dashboard/backend. In that layout, launch the native engine directly.
$backendMain = Join-Path $repoRoot "backend\src\main.py"
if (-not (Test-Path -LiteralPath $backendMain)) {
    $minerExe = Join-Path $repoRoot "unique-rx.exe"
    if (-not (Test-Path -LiteralPath $minerExe)) {
        throw "Backend e unique-rx.exe non trovati: pacchetto incompleto."
    }
    Write-Host "MSR abilitato. Avvio miner nativo in modalità portatile..."
    & $minerExe --mine $configPath
    exit $LASTEXITCODE
}

$venvDir = Join-Path $repoRoot ".venv"
$pythonExe = Join-Path $venvDir "Scripts\python.exe"
$requirements = Join-Path $repoRoot "requirements.txt"
if (-not (Test-Path -LiteralPath $pythonExe)) {
    New-Item -ItemType Directory -Path $venvDir -Force | Out-Null
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        & $launcher.Source -3 -m venv $venvDir
    } else {
        $basePython = Get-Command python -ErrorAction SilentlyContinue
        if (-not $basePython) { throw "Python 3 non trovato. Installa Python 3 e riprova." }
        & $basePython.Source -m venv $venvDir
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pythonExe)) {
        throw "Creazione ambiente Python non riuscita."
    }
}
& $pythonExe -c "import fastapi, uvicorn, pydantic" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installazione dipendenze backend nell'ambiente del progetto..."
    & $pythonExe -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) { throw "Installazione dipendenze backend non riuscita." }
}
& $pythonExe -c "import fastapi, uvicorn, pydantic"
if ($LASTEXITCODE -ne 0) { throw "Dipendenze backend non disponibili nell'ambiente .venv." }
New-Item -ItemType Directory -Path $logsDir -Force | Out-Null
$health = $null
try { $health = Invoke-RestMethod -Uri "$api/api/health" -TimeoutSec 2 } catch {}
if (-not $health -or $health.status -ne "ok") {
    $backend = Start-Process -FilePath $pythonExe -ArgumentList @("-m","uvicorn","backend.src.main:app","--host","127.0.0.1","--port","8000") -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logsDir "backend.stdout.log") -RedirectStandardError (Join-Path $logsDir "backend.stderr.log") -WindowStyle Hidden -PassThru
    Set-Content -LiteralPath (Join-Path $logsDir "backend.pid") -Value $backend.Id
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 200
        if ($backend.HasExited) { throw "Backend terminato. Controlla logs/backend.stderr.log." }
        try { $health = Invoke-RestMethod -Uri "$api/api/health" -TimeoutSec 2; if ($health.status -eq "ok") { break } } catch {}
    }
}
if (-not $health -or $health.status -ne "ok") { throw "Backend non raggiungibile su $api. Controlla logs/backend.stderr.log." }
$result = Invoke-RestMethod -Method Post -Uri "$api/api/miner/start" -TimeoutSec 10
Write-Host "Richiesta avvio miner inviata: $($result.status)"
