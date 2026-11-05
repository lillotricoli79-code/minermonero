[CmdletBinding()]
param(
  [ValidateSet('Configure','Start','Stop','Status')][string]$Action='Status',
  [string]$Pool,[string]$Wallet,[string]$Worker,[string]$Password,
  [ValidateRange(0,256)][int]$Threads=-1,
  [ValidateRange(0,5)][int]$CpuPriority=-1,
  [string]$CpuAffinity,
  [ValidateSet('auto','fast','light')][string]$RandomXMode,
  [ValidateRange(1,65535)][int]$ApiPort=-1,
  [switch]$Tls,[switch]$OpenCL,[switch]$CUDA,[switch]$DisableHugePages
)
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $MyInvocation.MyCommand.Path
$Data=Join-Path $Root 'data'; $CfgPath=Join-Path $Data 'config.json'
New-Item -ItemType Directory -Path $Data -Force | Out-Null
if(Test-Path -LiteralPath $CfgPath){$cfg=Get-Content -LiteralPath $CfgPath -Raw | ConvertFrom-Json}
else {
  $cfg=[pscustomobject]@{pool='pool.supportxmr.com:3333';wallet='';worker='rig01';password='x';threads=0;tls=$false;api_host='127.0.0.1';api_port=18088;api_token=([guid]::NewGuid().ToString('N')+[guid]::NewGuid().ToString('N'));opencl=$false;cuda=$false;xmrig_path='';cpu_priority=2;cpu_affinity='';randomx_mode='auto';huge_pages=$true}
}
function Save-Config {
  $tmp=$CfgPath+'.tmp'
  $json=$cfg | ConvertTo-Json -Depth 12
  [IO.File]::WriteAllText($tmp,$json,[Text.UTF8Encoding]::new($false))
  if(Test-Path -LiteralPath $CfgPath){Copy-Item -LiteralPath $CfgPath ($CfgPath+'.bak') -Force}
  Move-Item -LiteralPath $tmp -Destination $CfgPath -Force
}
function Get-MinerPath {
  if($cfg.xmrig_path -and (Test-Path -LiteralPath $cfg.xmrig_path)){return [string]$cfg.xmrig_path}
  $candidate=Join-Path $Root 'xmrig.exe'
  if(Test-Path -LiteralPath $candidate){return $candidate}
  throw 'xmrig.exe non trovato. Eseguire prima download_xmrig.bat.'
}
function Get-MinerProcess {
  $exe=Get-MinerPath
  return Get-CimInstance Win32_Process -Filter "Name='xmrig.exe'" | Where-Object { $_.ExecutablePath -eq $exe }
}
switch($Action){
  'Configure' {
    if($PSBoundParameters.ContainsKey('Pool')){if([string]::IsNullOrWhiteSpace($Pool)){throw 'Pool vuota.'};$cfg.pool=$Pool.Trim()}
    if($PSBoundParameters.ContainsKey('Wallet')){$cfg.wallet=$Wallet.Trim()}
    if($PSBoundParameters.ContainsKey('Worker')){$cfg.worker=$Worker.Trim()}
    if($PSBoundParameters.ContainsKey('Password')){$cfg.password=$Password}
    if($Threads -ge 0){$cfg.threads=$Threads}
    if($CpuPriority -ge 0){$cfg.cpu_priority=$CpuPriority}
    if($PSBoundParameters.ContainsKey('CpuAffinity')){$cfg.cpu_affinity=$CpuAffinity.Trim()}
    if($PSBoundParameters.ContainsKey('RandomXMode')){$cfg.randomx_mode=$RandomXMode}
    if($ApiPort -gt 0){$cfg.api_port=$ApiPort}
    if($Tls){$cfg.tls=$true};if($OpenCL){$cfg.opencl=$true};if($CUDA){$cfg.cuda=$true}
    if($DisableHugePages){$cfg.huge_pages=$false}
    if($cfg.api_host -notin @('127.0.0.1','localhost','::1')){throw 'API host deve rimanere locale.'}
    if([int]$cfg.api_port -lt 1 -or [int]$cfg.api_port -gt 65535){throw 'Porta API non valida.'}
    if([int]$cfg.threads -lt 0 -or [int]$cfg.threads -gt 256){throw 'Threads fuori intervallo.'}
    if([int]$cfg.cpu_priority -lt 0 -or [int]$cfg.cpu_priority -gt 5){throw 'Priorità CPU fuori intervallo 0-5.'}
    if($cfg.randomx_mode -notin @('auto','fast','light')){throw 'RandomX mode non valido.'}
    if($cfg.cpu_affinity -and $cfg.cpu_affinity -notmatch '^(0x)?[0-9A-Fa-f]+$'){throw 'Affinity deve essere una maschera esadecimale, es. 0xFF.'}
    Save-Config
    Write-Host 'Configurazione salvata. Wallet/password/token non vengono mostrati.'
    Write-Host "Pool=$($cfg.pool) Worker=$($cfg.worker) Threads=$($cfg.threads) Priority=$($cfg.cpu_priority) RandomX=$($cfg.randomx_mode)"
  }
  'Start' {
    if([string]::IsNullOrWhiteSpace([string]$cfg.wallet)){throw 'Configurare prima il wallet con -Action Configure -Wallet.'}
    $exe=Get-MinerPath
    if(Get-MinerProcess){Write-Host 'XMRig risulta già attivo.';break}
    $cpu=@{enabled=$true;'huge-pages'=[bool]$cfg.huge_pages;priority=[int]$cfg.cpu_priority}
    if([int]$cfg.threads -gt 0){$logical=[Math]::Max(1,[Environment]::ProcessorCount);$cpu['max-threads-hint']=[Math]::Max(1,[Math]::Min(100,[Math]::Round(100*[int]$cfg.threads/$logical)))}
    if($cfg.cpu_affinity){$cpu.affinity=[Convert]::ToInt64(([string]$cfg.cpu_affinity).Replace('0x',''),16)}
    $json=@{
      autosave=$false;background=$false;colors=$true
      http=@{enabled=$true;host='127.0.0.1';port=[int]$cfg.api_port;'access-token'=[string]$cfg.api_token;restricted=$true}
      cpu=$cpu;opencl=@{enabled=[bool]$cfg.opencl};cuda=@{enabled=[bool]$cfg.cuda}
      randomx=@{mode=[string]$cfg.randomx_mode}
      pools=@(@{url=[string]$cfg.pool;user=[string]$cfg.wallet;pass=[string]$cfg.password;'rig-id'=[string]$cfg.worker;keepalive=$true;tls=[bool]$cfg.tls;enabled=$true})
    } | ConvertTo-Json -Depth 12
    $configFile=Join-Path $Data 'xmrig.generated.json'
    [IO.File]::WriteAllText($configFile,$json,[Text.UTF8Encoding]::new($false))
    Start-Process -FilePath $exe -ArgumentList @('-c',$configFile) -WorkingDirectory (Split-Path -Parent $exe)
    Write-Host 'Avvio XMRig richiesto. Verificare Status per confermare connessione e hashrate.'
  }
  'Stop' {
    $processes=Get-MinerProcess
    if(-not $processes){Write-Host 'XMRig non risulta attivo.';break}
    foreach($p in $processes){Stop-Process -Id $p.ProcessId -ErrorAction Stop}
    Write-Host 'Richiesta di arresto inviata.'
  }
  'Status' {
    try {
      $headers=@{Authorization='Bearer '+[string]$cfg.api_token}
      $s=Invoke-RestMethod -Uri ("http://127.0.0.1:"+[int]$cfg.api_port+'/1/summary') -Headers $headers -TimeoutSec 3
      $h=$s.hashrate.total
      [pscustomobject]@{State='API XMRig attiva';Hash10s=$h[0];Hash60s=$h[1];Hash15m=$h[2];Accepted=$s.results.shares_good;Total=$s.results.shares_total;UptimeSeconds=$s.connection.uptime;Pool=$s.connection.pool}
    } catch {Write-Warning 'API non raggiungibile: miner fermo, avvio in corso o token/porta non corrispondenti.'}
  }
}
