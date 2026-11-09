$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$configPath = Join-Path $repoRoot "config.json"
$logsDir = Join-Path $repoRoot "logs"
$api = "http://127.0.0.1:8000"
if (-not (Test-Path -LiteralPath $configPath)) { throw "config.json non trovato. Copia config.example.json e configura wallet e pool." }

# Elevate only when the user's own configuration requests privileged features.
# MSR is never enabled or modified by this launcher.
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$needsAdmin = [bool](
    $config.cpu.'huge-pages' -or
    $config.cpu.'jit-huge-pages' -or
    $config.randomx.wrmsr
)
if ($needsAdmin) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $arguments -WorkingDirectory $repoRoot
        exit 0
    }
}

# Preserve the user's MSR choice exactly as stored in config.json.
# The native engine treats MSR as optional and continues without it when
# the feature is disabled, unsupported, or lacks permissions.

# A complete Windows package must start the native miner, backend API and
# dashboard as one coordinated application. Never silently fall back to a
# standalone miner: that produces the console shell but leaves the dashboard
# disconnected and its telemetry unavailable.
$backendMain = Join-Path $repoRoot "backend\src\main.py"
$dashboardIndex = Join-Path $repoRoot "dashboard\dist\index.html"
$minerExe = Join-Path $repoRoot "unique-rx.exe"
$requirements = Join-Path $repoRoot "requirements.txt"
$requiredFiles = @($backendMain, $dashboardIndex, $minerExe, $requirements)
$missingFiles = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingFiles.Count -gt 0) {
    throw ("Pacchetto UNIQUE RX incompleto: manca " + ($missingFiles -join ", ") +
        ". Estrai il pacchetto Windows completo e avvia scripts\windows\start.bat. " +
        "Non avvio il miner da solo per evitare la dashboard scollegata.")
}

$bundledPython = Join-Path $repoRoot "python\python.exe"
$venvDir = Join-Path $repoRoot ".venv"
$pythonExe = Join-Path $venvDir "Scripts\python.exe"
$requirements = Join-Path $repoRoot "requirements.txt"
if (Test-Path -LiteralPath $bundledPython) {
    # Release package: use its self-contained Python runtime and dependencies.
    $pythonExe = $bundledPython
} else {
    if (-not (Test-Path -LiteralPath $pythonExe)) {
        New-Item -ItemType Directory -Path $venvDir -Force | Out-Null
        $launcher = Get-Command py -ErrorAction SilentlyContinue
        if ($launcher) {
            & $launcher.Source -3 -m venv $venvDir
        } else {
            $basePython = Get-Command python -ErrorAction SilentlyContinue
            if (-not $basePython) { throw "Python 3 non trovato. Questo pacchetto non contiene il runtime Python." }
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
}
& $pythonExe -c "import fastapi, uvicorn, pydantic"
if ($LASTEXITCODE -ne 0) { throw "Dipendenze backend non disponibili nel runtime incluso." }

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

# Verify the built dashboard is present before opening the browser.
$dashboardIndex = Join-Path $repoRoot "dashboard\dist\index.html"
if (-not (Test-Path -LiteralPath $dashboardIndex)) {
    throw "Dashboard non compilata: manca dashboard\dist\index.html. Esegui scripts\windows\build.ps1."
}
$dashboardUrl = "$api/"
$dashboardReady = $false
for ($attempt = 0; $attempt -lt 15; $attempt++) {
    try {
        $page = Invoke-WebRequest -Uri $dashboardUrl -UseBasicParsing -TimeoutSec 3
        if ($page.StatusCode -eq 200 -and $page.Content.Contains('id="root"')) { $dashboardReady = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 300
}

# If a previous backend started before dashboard/dist existed, restart only
# the exact backend process recorded by this launcher.
if (-not $dashboardReady) {
    $pidFile = Join-Path $logsDir "backend.pid"
    if (Test-Path -LiteralPath $pidFile) {
        $ownedPid = 0
        [void][int]::TryParse((Get-Content -LiteralPath $pidFile -Raw).Trim(), [ref]$ownedPid)
        if ($ownedPid -gt 0) {
            $ownedProcess = Get-CimInstance Win32_Process -Filter "ProcessId = $ownedPid" -ErrorAction SilentlyContinue
            if ($ownedProcess -and $ownedProcess.CommandLine -match "backend\.src\.main:app") {
                Stop-Process -Id $ownedPid -Force -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
            }
        }
    }
    $backend = Start-Process -FilePath $pythonExe -ArgumentList @("-m","uvicorn","backend.src.main:app","--host","127.0.0.1","--port","8000") -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logsDir "backend.stdout.log") -RedirectStandardError (Join-Path $logsDir "backend.stderr.log") -WindowStyle Hidden -PassThru
    Set-Content -LiteralPath (Join-Path $logsDir "backend.pid") -Value $backend.Id
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 200
        if ($backend.HasExited) { throw "Backend riavviato ma terminato. Controlla logs/backend.stderr.log." }
        try {
            $page = Invoke-WebRequest -Uri $dashboardUrl -UseBasicParsing -TimeoutSec 2
            if ($page.StatusCode -eq 200 -and $page.Content.Contains('id="root"')) { $dashboardReady = $true; break }
        } catch {}
    }
}
if (-not $dashboardReady) {
    throw "Backend attivo ma dashboard non servita su $dashboardUrl. Controlla dashboard\dist\index.html e logs/backend.stderr.log."
}

# Keep the dashboard available even if miner startup reports an error.
try {
    $result = Invoke-RestMethod -Method Post -Uri "$api/api/miner/start" -TimeoutSec 10
    Write-Host "Richiesta avvio miner inviata: $($result.status)" -ForegroundColor Green
} catch {
    Write-Warning "Dashboard pronta; avvio miner non riuscito: $($_.Exception.Message)"
}
Set-Content -LiteralPath (Join-Path $logsDir "dashboard.url.txt") -Value $dashboardUrl -Encoding ascii
try {
    Start-Process -FilePath $dashboardUrl
    Write-Host "Dashboard UNIQUE RX aperta nel browser predefinito: $dashboardUrl" -ForegroundColor Green
} catch {
    Write-Warning "Dashboard attiva ma apertura automatica del browser non riuscita. Apri manualmente: $dashboardUrl"
}
