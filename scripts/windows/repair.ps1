# Compatibility entry point: keep one canonical repair/build procedure.
[CmdletBinding()]
param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ExpectedRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ActualRoot = (Resolve-Path $ProjectRoot).Path

if (-not [string]::Equals(
    $ActualRoot.TrimEnd('\'),
    $ExpectedRoot.TrimEnd('\'),
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw "Questo wrapper opera solo sul repository UNIQUE-RX che contiene lo script."
}
if ($SkipBuild) {
    throw "La vecchia modalità -SkipBuild è stata dismessa: usa scripts\windows\repair-miner.ps1 per l'unica procedura aggiornata."
}

$Canonical = Join-Path $PSScriptRoot "repair-miner.ps1"
if (-not (Test-Path -LiteralPath $Canonical)) {
    throw "Procedura canonica non trovata: $Canonical"
}

& $Canonical
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
