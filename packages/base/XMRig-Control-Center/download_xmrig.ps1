$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Api = 'https://api.github.com/repos/xmrig/xmrig/releases/latest'
$Headers = @{ 'User-Agent' = 'XMRig-Control-Center-Setup' }
$Release = Invoke-RestMethod -Uri $Api -Headers $Headers
$Asset = $Release.assets | Where-Object { $_.name -match 'win64.*\.zip$' -or $_.name -match 'windows.*64.*\.zip$' } | Select-Object -First 1
if (-not $Asset) { throw "Nessun archivio Windows x64 trovato nella release $($Release.tag_name). Scaricare manualmente da https://github.com/xmrig/xmrig/releases" }
$Zip = Join-Path $Root 'xmrig-release.zip'
$Dest = Join-Path $Root 'xmrig-engine'
Write-Host "Download XMRig $($Release.tag_name)..."
Invoke-WebRequest -Uri $Asset.browser_download_url -OutFile $Zip -Headers $Headers
if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
Expand-Archive -LiteralPath $Zip -DestinationPath $Dest -Force
$Exe = Get-ChildItem $Dest -Filter xmrig.exe -Recurse | Select-Object -First 1
if (-not $Exe) { throw 'xmrig.exe non trovato nell’archivio scaricato.' }
Copy-Item $Exe.FullName (Join-Path $Root 'xmrig.exe') -Force
Remove-Item $Zip -Force
Write-Host "XMRig installato: $(Join-Path $Root 'xmrig.exe')"
