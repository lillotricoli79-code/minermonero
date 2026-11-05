$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ConfigPath = Join-Path $Root 'data\config.json'

Write-Host '=== XMRig Control Center - Diagnostica ==='
Write-Host ("Windows: " + [Environment]::OSVersion.VersionString)
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
if ($cpu) { Write-Host ("CPU: " + $cpu.Name); Write-Host ("Core/Thread logici: " + $cpu.NumberOfCores + '/' + $cpu.NumberOfLogicalProcessors) }

$python = Get-Command py -ErrorAction SilentlyContinue
if ($python) {
    py -3 --version
} else {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { python --version } else { Write-Warning 'Python 3 non trovato.' }
}

$exe = Join-Path $Root 'xmrig.exe'
if (Test-Path -LiteralPath $exe) {
    Write-Host ("XMRig: presente (" + (Get-Item -LiteralPath $exe).Length + ' bytes)')
} else {
    Write-Warning 'xmrig.exe non trovato nella cartella del controller.'
}

if (Test-Path -LiteralPath $ConfigPath) {
    try {
        $cfg = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
        Write-Host ("Pool configurata: " + $cfg.pool)
        Write-Host ("Thread CPU: " + $cfg.threads)
        Write-Host ("API host/porta: " + $cfg.api_host + ':' + $cfg.api_port)
        if ($cfg.api_host -in @('127.0.0.1', 'localhost', '::1')) {
            $headers = @{ Authorization = 'Bearer ' + [string]$cfg.api_token }
            try {
                $summary = Invoke-RestMethod -Uri ("http://127.0.0.1:" + [int]$cfg.api_port + '/1/summary') -Headers $headers -TimeoutSec 2
                $rates = $summary.hashrate.total
                Write-Host ("API XMRig raggiungibile. Hashrate 10s/60s/15m: " + ($rates -join ' / '))
                Write-Host ("Share accettate/totali: " + $summary.results.shares_good + '/' + $summary.results.shares_total)
            } catch {
                Write-Host 'API XMRig non raggiungibile: miner fermo, porta non pronta o token non valido.'
            }
        } else {
            Write-Warning 'API host non locale: richiesta API saltata per sicurezza.'
        }
    } catch {
        Write-Warning ('Config JSON non leggibile: ' + $_.Exception.Message)
    }
} else {
    Write-Host 'Configurazione non ancora creata. Avvia la GUI o configure_xmrig.ps1.'
}

Write-Host 'Nota: questo controllo non misura temperature CPU/GPU o ventole; nessun valore viene simulato.'
