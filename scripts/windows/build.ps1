$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"
$dashboardDir = Join-Path $repoRoot "dashboard"

# Obtain only the official, signed PawnIO Intel module and pin its exact digest.
$msrModulePath = Join-Path $engineDir "msr\IntelMSR.bin"
$expectedMsrSha256 = "d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f"
if (-not (Test-Path -LiteralPath $msrModulePath)) {
    $archive = Join-Path $env:TEMP "unique-rx-pawnio-0.2.11.zip"
    $extract = Join-Path $env:TEMP "unique-rx-pawnio-0.2.11"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $msrModulePath) | Out-Null
    Invoke-WebRequest -Uri "https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip" -OutFile $archive
    Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
    $downloadedModule = Get-ChildItem -LiteralPath $extract -Filter "IntelMSR.bin" -File -Recurse | Select-Object -First 1
    if (-not $downloadedModule) { throw "IntelMSR.bin assente dal pacchetto PawnIO 0.2.11." }
    $downloadedHash = (Get-FileHash -LiteralPath $downloadedModule.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($downloadedHash -ne $expectedMsrSha256) { throw "Hash SHA-256 IntelMSR.bin non valido: $downloadedHash" }
    Copy-Item -LiteralPath $downloadedModule.FullName -Destination $msrModulePath -Force
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
}
$installedMsrHash = (Get-FileHash -LiteralPath $msrModulePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($installedMsrHash -ne $expectedMsrSha256) { throw "IntelMSR.bin presente nel progetto ma SHA-256 non corrispondente: $installedMsrHash" }
Write-Host "PawnIO IntelMSR 0.2.11 verificato: $installedMsrHash" -ForegroundColor Green

# The release build must never silently reuse a stale dashboard bundle.
$node = Get-Command node -ErrorAction SilentlyContinue
$npm = Get-Command npm -ErrorAction SilentlyContinue
if (-not $node -or -not $npm) {
    throw "Node.js e npm sono obbligatori per la build completa della dashboard. Installa Node.js 20.19+ o 22.12+."
}
$nodeVersionText = (& $node.Source --version).Trim().TrimStart("v")
try { $nodeVersion = [version]$nodeVersionText } catch { throw "Versione Node.js non riconosciuta: $nodeVersionText" }
$nodeSupported = ($nodeVersion.Major -eq 20 -and $nodeVersion -ge [version]"20.19") -or ($nodeVersion -ge [version]"22.12")
if (-not $nodeSupported) { throw "Node.js $nodeVersionText non supportato da Vite. Usa Node.js 20.19+ o 22.12+." }

# Prepare the backend environment before starting the final native build.
$venvDir = Join-Path $repoRoot ".venv"
$pythonExe = Join-Path $venvDir "Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pythonExe)) {
    New-Item -ItemType Directory -Path $venvDir -Force | Out-Null
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) { & $launcher.Source -3 -m venv $venvDir }
    else {
        $basePython = Get-Command python -ErrorAction SilentlyContinue
        if (-not $basePython) { throw "Python 3 è obbligatorio per backend e test." }
        & $basePython.Source -m venv $venvDir
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pythonExe)) { throw "Creazione ambiente Python non riuscita." }
}
& $pythonExe -c "import fastapi, uvicorn, pydantic" 2>$null
if ($LASTEXITCODE -ne 0) {
    & $pythonExe -m pip install -r (Join-Path $repoRoot "requirements.txt")
    if ($LASTEXITCODE -ne 0) { throw "Installazione dipendenze backend non riuscita." }
}
& $pythonExe -c "import fastapi, uvicorn, pydantic"
if ($LASTEXITCODE -ne 0) { throw "Dipendenze backend non disponibili." }

Write-Host "=== BUILD DASHBOARD ===" -ForegroundColor Cyan
& $npm.Source ci --prefix $dashboardDir
if ($LASTEXITCODE -ne 0) { throw "Installazione dipendenze dashboard fallita." }
& $npm.Source run build --prefix $dashboardDir
if ($LASTEXITCODE -ne 0) { throw "Build dashboard fallita." }
if (-not (Test-Path (Join-Path $dashboardDir "dist\index.html"))) { throw "Build dashboard terminata senza dashboard\dist\index.html." }

# MSYS2/setup-msys2 installs its distribution under RUNNER_TEMP\msys64.
# Its command wrapper lives in RUNNER_TEMP\setup-msys2; those are different
# directories. Probe both, plus the conventional locations and PATH compilers.
$mingwPrefixes = @()
if ($env:RUNNER_TEMP) {
    $mingwPrefixes += (Join-Path $env:RUNNER_TEMP "msys64\mingw64")
    $mingwPrefixes += (Join-Path $env:RUNNER_TEMP "setup-msys2\msys64\mingw64")
}
$cygpath = Get-Command cygpath -ErrorAction SilentlyContinue
if ($cygpath) {
    $translatedPrefix = (& $cygpath.Source -w /mingw64 2>$null | Select-Object -First 1)
    if ($translatedPrefix) { $mingwPrefixes += $translatedPrefix.Trim() }
}
$mingwPrefixes += @("C:\msys64\mingw64", "C:\mingw64")

$pathCompilers = @(Get-Command g++ -All -ErrorAction SilentlyContinue)
foreach ($compiler in $pathCompilers) {
    $mingwPrefixes += (Split-Path -Parent (Split-Path -Parent $compiler.Source))
}
$mingwCandidates = @($mingwPrefixes | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
$mingwPrefix = $null
foreach ($candidatePrefix in $mingwCandidates) {
    $candidateGxx = Join-Path $candidatePrefix "bin\g++.exe"
    $candidateOpenSsl = Join-Path $candidatePrefix "include\openssl\ssl.h"
    if (-not (Test-Path -LiteralPath $candidateGxx) -or -not (Test-Path -LiteralPath $candidateOpenSsl)) { continue }
    $versionText = (& $candidateGxx -dumpfullversion -dumpversion 2>$null | Select-Object -First 1)
    try { $compilerVersion = [version]$versionText.Trim() } catch { continue }
    if ($compilerVersion.Major -lt 13) {
        Write-Host "Ignoro GCC $compilerVersion in ${candidatePrefix}: richiesto GCC 13+." -ForegroundColor Yellow
        continue
    }
    $mingwPrefix = $candidatePrefix
    break
}
if (-not $mingwPrefix) {
    $compilerList = ($pathCompilers | ForEach-Object { $_.Source }) -join "; "
    throw "Toolchain MinGW GCC 13+ con OpenSSL non individuata. Prefissi verificati: $($mingwCandidates -join '; '). Compiler in PATH: $compilerList"
}
$msysBin = Join-Path $mingwPrefix "bin"
$env:PATH = "$msysBin$([IO.Path]::PathSeparator)$env:PATH"
Write-Host "Compiler MinGW selezionato: $(Join-Path $msysBin 'g++.exe')" -ForegroundColor Green
& (Join-Path $msysBin "g++.exe") --version | Select-Object -First 1

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { throw "CMake è obbligatorio. Installa CMake e riapri PowerShell." }
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if (-not $ctest) { throw "CTest non trovato: reinstalla CMake con i componenti di testing." }
$gxx = Get-Command (Join-Path $mingwPrefix "bin\g++.exe")
$ninjaPath = Join-Path $mingwPrefix "bin\ninja.exe"
$makePath = Join-Path $mingwPrefix "bin\mingw32-make.exe"
if (Test-Path -LiteralPath $ninjaPath) { $ninja = Get-Command $ninjaPath; $generator = "Ninja"; $buildDir = Join-Path $engineDir "build-mingw-ninja" }
elseif (Test-Path -LiteralPath $makePath) { $mingwMake = Get-Command $makePath; $generator = "MinGW Makefiles"; $buildDir = Join-Path $engineDir "build-mingw-make" }
else { throw "Toolchain MinGW rilevata ma manca Ninja o mingw32-make." }

$runtimeSearchDirs = @((Join-Path $mingwPrefix "bin"))
$opensslHeader = Join-Path $mingwPrefix "include\openssl\ssl.h"
if (-not (Test-Path -LiteralPath $opensslHeader)) { throw "OpenSSL MinGW non trovato in $mingwPrefix." }
Write-Host "Toolchain: MinGW-w64 + $generator."
$configureArgs = @(
    "-S", $engineDir, "-B", $buildDir, "-G", $generator,
    "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
    "-DCMAKE_C_COMPILER=$(Join-Path $mingwPrefix 'bin\gcc.exe')",
    "-DCMAKE_CXX_COMPILER=$($gxx.Source)",
    "-DCMAKE_PREFIX_PATH=$mingwPrefix", "-DOPENSSL_ROOT_DIR=$mingwPrefix"
)
& $cmake.Source @configureArgs
if ($LASTEXITCODE -ne 0) { throw "Configurazione CMake MinGW fallita." }

Write-Host "=== COMPILAZIONE NATIVA RELEASE ===" -ForegroundColor Cyan
& $cmake.Source --build $buildDir --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Compilazione motore/test nativi fallita." }

Write-Host "=== TEST BACKEND ===" -ForegroundColor Cyan
Push-Location $repoRoot
try {
    & $pythonExe -m unittest backend.tests.test_main
    if ($LASTEXITCODE -ne 0) { throw "Test backend falliti." }
} finally { Pop-Location }

Write-Host "=== TEST NATIVI ===" -ForegroundColor Cyan
& $ctest.Source --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Uno o più test nativi sono falliti." }

$exeCandidates = @((Join-Path $buildDir "unique-rx.exe"), (Join-Path $buildDir "Release\unique-rx.exe"))
$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $exe) { throw "Build completata ma unique-rx.exe non trovato nella cartella di output." }

# Stage all non-system MinGW DLL dependencies beside the executable.
$objdump = Get-Command (Join-Path $mingwPrefix "bin\objdump.exe") -ErrorAction SilentlyContinue
if (-not $objdump) { throw "objdump MinGW non trovato: impossibile verificare le DLL richieste dal miner." }
$exeDirectory = Split-Path -Parent $exe
$queue = [System.Collections.Generic.Queue[string]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$queue.Enqueue($exe)
$systemDlls = @("advapi32.dll","bcrypt.dll","bcryptprimitives.dll","cfgmgr32.dll","comctl32.dll","comdlg32.dll","gdi32.dll","imm32.dll","iphlpapi.dll","kernel32.dll","kernelbase.dll","mpr.dll","msvcrt.dll","ntdll.dll","ole32.dll","oleaut32.dll","powrprof.dll","psapi.dll","rpcrt4.dll","sechost.dll","shell32.dll","shlwapi.dll","user32.dll","version.dll","winmm.dll","ws2_32.dll","wldap32.dll","wtsapi32.dll","ucrtbase.dll","crypt32.dll","cryptbase.dll","cryptsp.dll","ncrypt.dll","secur32.dll")
$systemDllSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($systemDll in $systemDlls) { [void]$systemDllSet.Add($systemDll) }
while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    if (-not $seen.Add($current)) { continue }
    $rawDependencies = & $objdump.Source -p $current 2>$null
    foreach ($dependencyLine in $rawDependencies) {
        if ($dependencyLine -notmatch 'DLL Name:\s*(\S+)') { continue }
        $dependency = $Matches[1]
        if ($dependency -imatch '^(api-ms-win-|ext-ms-win-)' -or $systemDllSet.Contains($dependency)) { continue }
        $resolved = Join-Path $mingwPrefix "bin\$dependency"
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { throw "Dipendenza DLL MinGW non risolta: $dependency (richiesta da $current)." }
        $destination = Join-Path $exeDirectory $dependency
        if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash $resolved -Algorithm SHA256).Hash -ne (Get-FileHash $destination -Algorithm SHA256).Hash) {
            Copy-Item -LiteralPath $resolved -Destination $destination -Force
        }
        $queue.Enqueue($destination)
    }
}

# Produce a clean, portable folder locally as well as in GitHub Actions.
$packageDir = Join-Path $repoRoot "release\unique-rx-windows-x64"
if (Test-Path -LiteralPath $packageDir) { Remove-Item -LiteralPath $packageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item -LiteralPath $exe -Destination $packageDir
Get-ChildItem -LiteralPath $exeDirectory -Filter "*.dll" -File | Copy-Item -Destination $packageDir -Force
$packageMsrDir = Join-Path $packageDir "msr"
New-Item -ItemType Directory -Force -Path $packageMsrDir | Out-Null
$builtMsrModule = Join-Path $exeDirectory "msr\IntelMSR.bin"
if (-not (Test-Path -LiteralPath $builtMsrModule)) { throw "Il modulo IntelMSR verificato non è stato copiato accanto all'eseguibile." }
Copy-Item -LiteralPath $builtMsrModule -Destination (Join-Path $packageMsrDir "IntelMSR.bin")
Copy-Item -LiteralPath (Join-Path $engineDir "msr\PawnIO-COPYING") -Destination (Join-Path $packageMsrDir "COPYING")
$exeHash = (Get-FileHash -LiteralPath (Join-Path $packageDir "unique-rx.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
$moduleHash = (Get-FileHash -LiteralPath (Join-Path $packageMsrDir "IntelMSR.bin") -Algorithm SHA256).Hash.ToLowerInvariant()
if ($moduleHash -ne $expectedMsrSha256) { throw "Hash del modulo MSR nel pacchetto non valido." }
Set-Content -LiteralPath (Join-Path $packageDir "SHA256SUMS.txt") -Encoding ascii -Value @("$exeHash  unique-rx.exe", "$moduleHash  msr/IntelMSR.bin")
Write-Host ""
Write-Host "[OK] Dashboard, backend, motore nativo e suite automatica completati." -ForegroundColor Green
Write-Host "Miner: $exe"
Write-Host "Pacchetto portatile: $packageDir"
Write-Host "Dashboard: $(Join-Path $dashboardDir 'dist\index.html')"
Write-Host "Log Stratum: $(Join-Path $repoRoot 'unique-rx-stratum.log')"
