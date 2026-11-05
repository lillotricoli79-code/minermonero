[CmdletBinding()]
param([switch]$AsJson)
$ErrorActionPreference='Stop'
# Optional real sensor provider: LibreHardwareMonitor must be running with WMI enabled.
$namespace='root\LibreHardwareMonitor'
try {
  $sensors=Get-CimInstance -Namespace $namespace -ClassName Sensor -ErrorAction Stop
  $items=@($sensors | Where-Object {$_.SensorType -in @('Temperature','Fan','Load','Power','Clock')} |
    ForEach-Object {
      [pscustomobject]@{
        Name=[string]$_.Name
        Hardware=[string]$_.Parent
        Type=[string]$_.SensorType
        Value=if($null -ne $_.Value){[double]$_.Value}else{$null}
        Unit=switch([string]$_.SensorType){'Temperature'{'°C'} 'Fan'{'RPM'} 'Load'{'%'} 'Power'{'W'} 'Clock'{'MHz'} default{''}}
        Identifier=[string]$_.Identifier
      }
    })
  if($AsJson){@{available=$true;provider='LibreHardwareMonitor WMI';sensors=$items} | ConvertTo-Json -Depth 6}
  else {
    Write-Host 'Sensori reali - LibreHardwareMonitor'
    if(-not $items.Count){Write-Warning 'Provider presente ma nessun sensore compatibile restituito.'}
    $items | Format-Table Hardware,Name,Type,Value,Unit -AutoSize
  }
} catch {
  $result=@{available=$false;provider='LibreHardwareMonitor WMI';sensors=@();message='Sensori non disponibili. Avvia LibreHardwareMonitor, abilita WMI e riprova.'}
  if($AsJson){$result | ConvertTo-Json -Depth 4}
  else {Write-Warning $result.message;Write-Host 'Nessuna temperatura, ventola o potenza viene simulata.'}
}
