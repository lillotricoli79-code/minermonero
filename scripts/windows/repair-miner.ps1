# UNIQUE-RX safe repair/build script
# Updates only the existing main branch, builds the miner and runs the test suite.
# It never edits config.json, wallet credentials, pool settings, or creates branches.

$ErrorActionPreference = "Stop"

function Stop-Repair([string]$Message) {
    Write-Host "[ERRORE] $Message" -ForegroundColor Red
    exit 1
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Git = Get-Command git -ErrorAction SilentlyContinue
$CMake = Get-Command cmake -ErrorAction SilentlyContinue
$CTest = Get-Command ctest -ErrorAction SilentlyContinue

if (-not $Git) { Stop-Repair "Git non trovato nel PATH." }
if (-not $CMake) { Stop-Repair "CMake non trovato nel PATH." }
if (-not $CTest) { Stop-Repair "CTest non trovato nel PATH." }

$Branch = (& git -C $Root branch --show-current 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or $Branch -ne "main") {
    Stop-Repair "Il repository deve essere sul branch main. Branch attuale: '$Branch'. Nessun branch viene creato o cambiato."
}

$Remote = (& git -C $Root remote get-url origin 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or $Remote -notmatch "github\.com[:/]lillotricoli79-code/minermonero(?:\.git)?$") {
    Stop-Repair "Il remote origin non corrisponde a lillotricoli79-code/minermonero."
}

$Dirty = @(& git -C $Root status --porcelain)
if ($LASTEXITCODE -ne 0) { Stop-Repair "Impossibile leggere lo stato Git." }
if ($Dirty.Count -gt 0) {
    Write-Host "File locali modificati:" -ForegroundColor Yellow
    $Dirty | ForEach-Object { Write-Host "  $_" }
    Stop-Repair "Ci sono modifiche locali non salvate. Per sicurezza non le sovrascrivo: salva o sposta le modifiche e rilancia."
}

Write-Host "=== AGGIORNAMENTO SICURO MAIN ===" -ForegroundColor Cyan
& git -C $Root fetch origin main
if ($LASTEXITCODE -ne 0) { Stop-Repair "git fetch origin main non riuscito." }

& git -C $Root merge --ff-only origin/main
if ($LASTEXITCODE -ne 0) { Stop-Repair "Aggiornamento fast-forward non riuscito. Nessun reset/force è stato eseguito." }

$Client = Join-Path $Root "engine\src\stratum\client.cpp"
$Source = Get-Content -LiteralPath $Client -Raw
$Required = @(
    'log_event("SUBMIT_SENT", submit_request);',
    'log_event("SUBMIT_RESPONSE", message);',
    'log_event("SHARE_ACCEPTED"'
)
foreach ($Marker in $Required) {
    if (-not $Source.Contains($Marker)) {
        Stop-Repair "La correzione Stratum attesa non è presente: $Marker"
    }
}

Write-Host "OK: gestione submit allineata al comportamento XMRig e log diagnostici presenti." -ForegroundColor Green

# Keep the user's installed MSYS2 toolchain available to the unified release builder.
$MsysBin = "C:\msys64\mingw64\bin"
if (Test-Path $MsysBin) {
    $env:PATH = "$MsysBin;$env:PATH"
}

Write-Host "=== BUILD FINALE COMPLETA ===" -ForegroundColor Cyan
$BuildScript = Join-Path $Root "scripts\windows\build.ps1"
if (-not (Test-Path -LiteralPath $BuildScript)) {
    Stop-Repair "Script di build completa non trovato: $BuildScript"
}
& $BuildScript
if ($LASTEXITCODE -ne 0) {
    Stop-Repair "Build completa, test backend o test nativi non riusciti."
}

Write-Host ""
Write-Host "[OK] Build completa terminata." -ForegroundColor Green
Write-Host "La configurazione config.json e le credenziali pool non sono state modificate."
Write-Host "Il mining non viene avviato."
