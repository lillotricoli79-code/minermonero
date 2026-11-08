[CmdletBinding()]
param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$LogDir = Join-Path $env:TEMP "UNIQUE-RX-Repair"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Log = Join-Path $LogDir "repair-$Stamp.log"
Start-Transcript -Path $Log -Force | Out-Null

function Stop-Repair([string]$Message) {
    Write-Host ""
    Write-Host "ERRORE: $Message" -ForegroundColor Red
    Write-Host "Log: $Log" -ForegroundColor Yellow
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}

try {
    $Root = (Resolve-Path $ProjectRoot).Path
    $Client = Join-Path $Root "engine\src\stratum\client.cpp"
    $Loopback = Join-Path $Root "engine\tests\stratum\loopback_test.cpp"
    $Build = Join-Path $Root "engine\build"
    foreach ($Path in @($Client, $Loopback, (Join-Path $Root "engine\CMakeLists.txt"))) {
        if (-not (Test-Path $Path -PathType Leaf)) { Stop-Repair "File richiesto mancante: $Path" }
    }

    Write-Host "UNIQUE-RX - riparazione controllata" -ForegroundColor Cyan
    Write-Host "Progetto: $Root"
    Write-Host "Log e backup: $LogDir"

    $Backup = Join-Path $LogDir "backup-$Stamp"
    New-Item -ItemType Directory -Force -Path $Backup | Out-Null
    Copy-Item $Client (Join-Path $Backup "client.cpp.original")
    Copy-Item $Loopback (Join-Path $Backup "loopback_test.cpp.original")

    $ClientText = [IO.File]::ReadAllText($Client)
    $OldLogin = @'
                transport.send_json({{"id", 1}, {"jsonrpc", "2.0"}, {"method", "login"},
                    {"params", {{"login", pool.wallet}, {"pass", pool.password},
                                 {"rigid", pool.worker}, {"agent", "UNIQUE-RX/0.1"},
                                 {"algo", nlohmann::json::array({"rx/0"})}}}});
'@
    $WorkingLogin = @'
                transport.send_json({{"id", 1}, {"jsonrpc", "2.0"}, {"method", "login"},
                    {"params", {{"login", pool.wallet}, {"pass", pool.password},
                                 {"agent", "XMRig/6.26.0 gcc/15.1.0 (built for Windows x86-64, 64 bit)"},
                                 {"rigid", pool.worker}}}});
'@
    if ($ClientText.Contains($OldLogin)) {
        $ClientText = $ClientText.Replace($OldLogin, $WorkingLogin)
        [IO.File]::WriteAllText($Client, $ClientText, [Text.UTF8Encoding]::new($false))
        Write-Host "[OK] Ripristinato il login HashVault già verificato localmente."
    } elseif ($ClientText.Contains('XMRig/6.26.0 gcc/15.1.0 (built for Windows x86-64, 64 bit)')) {
        Write-Host "[OK] Login HashVault compatibile già presente: lasciato invariato."
    } else {
        Stop-Repair "Blocco login diverso dalle versioni conosciute. Nessuna modifica automatica applicata."
    }

    $LoopText = [IO.File]::ReadAllText($Loopback)
    $OldAssertion = @'
                    login["params"].value("rigid", std::string()) == "test" &&
                    login["params"].contains("algo") &&
                    login["params"]["algo"].is_array() &&
                    login["params"]["algo"] == nlohmann::json::array({"rx/0"});
'@
    $NewAssertion = @'
                    login["params"].value("rigid", std::string()) == "test" &&
                    login["params"].value("agent", std::string()) ==
                        "XMRig/6.26.0 gcc/15.1.0 (built for Windows x86-64, 64 bit)" &&
                    !login["params"].contains("algo");
'@
    if ($LoopText.Contains($OldAssertion)) {
        $LoopText = $LoopText.Replace($OldAssertion, $NewAssertion)
        [IO.File]::WriteAllText($Loopback, $LoopText, [Text.UTF8Encoding]::new($false))
        Write-Host "[OK] Test loopback allineato al login effettivo."
    } elseif ($LoopText.Contains('XMRig/6.26.0 gcc/15.1.0 (built for Windows x86-64, 64 bit)')) {
        Write-Host "[OK] Test loopback già allineato."
    } else {
        Stop-Repair "Asserzione login del test loopback non riconosciuta. Backup conservato; fermato senza sovrascrivere."
    }

    # Registra target e intervallo nonce ricevuti dalla pool, senza alterare i dati.
    $ClientText = [IO.File]::ReadAllText($Client)
    $JobMarker = '        const auto install_job = [&](Job next_job) {'
    $JobLog = @'
        const auto install_job = [&](Job next_job) {
            std::cout << "Job details: target=" << next_job.target
                      << " nonce_start=" << next_job.nonce
                      << " nonce_mask=" << next_job.nonce_mask
                      << " nonce_prefix=" << next_job.nonce_fixed << '\n';
'@
    if ($ClientText.Contains($JobMarker) -and -not $ClientText.Contains("Job details: target=")) {
        $ClientText = $ClientText.Replace($JobMarker, $JobLog.TrimEnd([char]13, [char]10))
        [IO.File]::WriteAllText($Client, $ClientText, [Text.UTF8Encoding]::new($false))
        Write-Host "[OK] Aggiunto log dei parametri reali del job."
    } elseif ($ClientText.Contains("Job details: target=")) {
        Write-Host "[OK] Log parametri job già presente."
    } else {
        Stop-Repair "Punto di inserimento log job non riconosciuto."
    }

    if ($SkipBuild) {
        Write-Host "Patch applicate. Build/test saltati su richiesta." -ForegroundColor Yellow
    } else {
        if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Stop-Repair "CMake non trovato nel PATH." }
        if (-not (Test-Path $Build -PathType Container)) { Stop-Repair "Directory engine\build assente. Configurare prima CMake secondo README." }

        Write-Host ""
        Write-Host "=== BUILD FINALE ===" -ForegroundColor Cyan
        & cmake --build $Build --config Release --parallel 4
        if ($LASTEXITCODE -ne 0) { Stop-Repair "Build fallita con codice $LASTEXITCODE." }

        Write-Host ""
        Write-Host "=== TEST FINALI ===" -ForegroundColor Cyan
        & ctest --test-dir $Build -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { Stop-Repair "Uno o più test sono falliti (codice $LASTEXITCODE)." }
        Write-Host ""
        Write-Host "[OK] Build e test automatici completati." -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "ATTENZIONE: lo script non avvia il mining e non scrive registri MSR."
    Write-Host "Build e test loopback NON provano una share accettata da HashVault."
    Write-Host "Controllare poi sul pool reale i contatori found, submitted e accepted."
    Write-Host "Log: $Log"
    Write-Host "Backup: $Backup"
    Stop-Transcript | Out-Null
}
catch {
    Write-Host ""
    Write-Host "ERRORE NON GESTITO: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Log: $Log" -ForegroundColor Yellow
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
