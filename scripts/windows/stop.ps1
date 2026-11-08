$ErrorActionPreference = "Stop"
$api = "http://127.0.0.1:8000"
try {
    $result = Invoke-RestMethod -Method Post -Uri "$api/api/miner/stop" -TimeoutSec 40
    Write-Host "Arresto miner richiesto: $($result.status)"
} catch {
    throw "Arresto non riuscito tramite backend locale. Verifica start.ps1 e logs/backend.stderr.log. Dettaglio: $($_.Exception.Message)"
}
