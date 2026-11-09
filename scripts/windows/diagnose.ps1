$ErrorActionPreference = "Continue"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Write-Host "UNIQUE RX - Diagnostica locale"
Write-Host "Repository: $repoRoot"
$config = Join-Path $repoRoot "config.json"
if (Test-Path -LiteralPath $config) {
    try {
        $parsed = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
        Write-Host "[OK] config.json leggibile"
        $enabled = @($parsed.pools | Where-Object { $_.enabled -ne $false })
        if ($enabled.Count -gt 0 -and $enabled[0].user -and $enabled[0].url) { Write-Host "[OK] Pool e wallet configurati" }
        else { Write-Warning "Configura un pool abilitato e il wallet." }
    } catch { Write-Warning "config.json non valido: $($_.Exception.Message)" }
} else { Write-Warning "config.json assente: copia config.example.json e configura wallet/pool." }
$pythonExe = Join-Path $repoRoot ".venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pythonExe)) {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { $pythonExe = $python.Source }
}
if (Test-Path -LiteralPath $pythonExe) {
    & $pythonExe --version
    & $pythonExe -c "import fastapi, uvicorn, pydantic"
    if ($LASTEXITCODE -eq 0) { Write-Host "[OK] FastAPI, Uvicorn e Pydantic disponibili" }
    else { Write-Warning "Dipendenze mancanti: scripts\windows\start.ps1 le installa nell'ambiente .venv." }
} else { Write-Warning "Python non trovato." }
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { & $cmake.Source --version | Select-Object -First 1 } else { Write-Warning "CMake non trovato." }
$bins = @(
    (Join-Path $repoRoot "unique-rx.exe"),
    (Join-Path $repoRoot "engine\build\unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-mingw-ninja\unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-mingw-make\unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-mingw\unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-msvc\unique-rx.exe"),
    (Join-Path $repoRoot "engine\build-msvc\Release\unique-rx.exe")
)
$miner = $bins | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($miner) { Write-Host "[OK] Motore nativo: $miner" } else { Write-Warning "Eseguibile assente. Compila con scripts/windows/build.ps1." }
try { $health = Invoke-RestMethod -Uri "http://127.0.0.1:8000/api/health" -TimeoutSec 2; Write-Host "[OK] Backend: $($health.status)"; Invoke-RestMethod -Uri "http://127.0.0.1:8000/api/status" -TimeoutSec 3 | ConvertTo-Json -Depth 6 } catch { Write-Host "Backend locale non avviato." }
