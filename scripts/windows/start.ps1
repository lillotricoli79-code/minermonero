$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$configPath = Join-Path $repoRoot "config.json"
$logsDir = Join-Path $repoRoot "logs"
$api = "http://127.0.0.1:8000"
if (-not (Test-Path -LiteralPath $configPath)) { throw "config.json non trovato. Copia config.example.json e configura wallet e pool." }
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { throw "Python 3 non trovato nel PATH." }
& $python.Source -c "import fastapi, uvicorn"
if ($LASTEXITCODE -ne 0) { throw "Dipendenze backend mancanti: python -m pip install -r requirements.txt" }
New-Item -ItemType Directory -Path $logsDir -Force | Out-Null
$health = $null
try { $health = Invoke-RestMethod -Uri "$api/api/health" -TimeoutSec 2 } catch {}
if (-not $health -or $health.status -ne "ok") {
    $backend = Start-Process -FilePath $python.Source -ArgumentList @("-m","uvicorn","backend.src.main:app","--host","127.0.0.1","--port","8000") -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logsDir "backend.stdout.log") -RedirectStandardError (Join-Path $logsDir "backend.stderr.log") -WindowStyle Hidden -PassThru
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
