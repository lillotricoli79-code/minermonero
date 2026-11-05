param(
    [string]$Pool,
    [string]$Wallet,
    [string]$Worker,
    [string]$Password,
    [int]$Threads = -1,
    [int]$ApiPort = -1,
    [switch]$EnableTls,
    [switch]$DisableTls,
    [switch]$EnableOpenCL,
    [switch]$DisableOpenCL,
    [switch]$EnableCuda,
    [switch]$DisableCuda
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$DataDir = Join-Path $Root 'data'
$ConfigPath = Join-Path $DataDir 'config.json'
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

if (Test-Path -LiteralPath $ConfigPath) {
    $cfg = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
} else {
    $cfg = [pscustomobject]@{
        pool = 'pool.supportxmr.com:3333'
        wallet = ''
        worker = 'rig01'
        password = 'x'
        threads = 0
        tls = $false
        api_host = '127.0.0.1'
        api_port = 18088
        api_token = ([guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N'))
        opencl = $false
        cuda = $false
        xmrig_path = ''
    }
}

if ($PSBoundParameters.ContainsKey('Pool')) {
    if ([string]::IsNullOrWhiteSpace($Pool)) { throw 'Pool non può essere vuota.' }
    $cfg.pool = $Pool.Trim()
}
if ($PSBoundParameters.ContainsKey('Wallet')) { $cfg.wallet = $Wallet.Trim() }
if ($PSBoundParameters.ContainsKey('Worker')) { $cfg.worker = $Worker.Trim() }
if ($PSBoundParameters.ContainsKey('Password')) { $cfg.password = $Password }
if ($Threads -ge 0) { $cfg.threads = $Threads }
if ($ApiPort -ge 0) {
    if ($ApiPort -lt 1 -or $ApiPort -gt 65535) { throw 'ApiPort deve essere tra 1 e 65535.' }
    $cfg.api_port = $ApiPort
}
if ($EnableTls -and $DisableTls) { throw 'Scegliere EnableTls oppure DisableTls, non entrambi.' }
if ($EnableOpenCL -and $DisableOpenCL) { throw 'Scegliere EnableOpenCL oppure DisableOpenCL, non entrambi.' }
if ($EnableCuda -and $DisableCuda) { throw 'Scegliere EnableCuda oppure DisableCuda, non entrambi.' }
if ($EnableTls) { $cfg.tls = $true }
if ($DisableTls) { $cfg.tls = $false }
if ($EnableOpenCL) { $cfg.opencl = $true }
if ($DisableOpenCL) { $cfg.opencl = $false }
if ($EnableCuda) { $cfg.cuda = $true }
if ($DisableCuda) { $cfg.cuda = $false }

if ($cfg.api_host -notin @('127.0.0.1', 'localhost', '::1')) {
    throw 'Per sicurezza api_host deve rimanere su loopback.'
}
if ([int]$cfg.api_port -lt 1 -or [int]$cfg.api_port -gt 65535) {
    throw 'La porta API salvata non è valida.'
}

if (Test-Path -LiteralPath $ConfigPath) {
    $Backup = Join-Path $DataDir ('config.json.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
    Copy-Item -LiteralPath $ConfigPath -Destination $Backup
    Write-Host "Backup configurazione: $Backup"
}
$json = $cfg | ConvertTo-Json -Depth 8
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($ConfigPath, $json + [Environment]::NewLine, $utf8NoBom)

Write-Host 'Configurazione aggiornata.'
Write-Host ("Pool: " + $cfg.pool)
Write-Host ("Worker: " + $cfg.worker)
Write-Host ("Thread CPU: " + $cfg.threads + ' (0 = automatico)')
Write-Host ("TLS: " + $cfg.tls + ' | OpenCL: ' + $cfg.opencl + ' | CUDA: ' + $cfg.cuda)
Write-Host 'Wallet, password e token API non vengono mostrati.'
