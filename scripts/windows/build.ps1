$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"
$dashboardDir = Join-Path $repoRoot "dashboard"

# WinRing0 is optional and user-supplied. No driver is downloaded, installed or modified.
# Accept the driver beside an existing miner or in the canonical engine\msr folder.
$winRingSource = Join-Path $engineDir "msr\WinRing0x64.sys"
$winRingCandidates = @(
    $winRingSource,
    (Join-Path $repoRoot "WinRing0x64.sys"),
    (Join-Path $engineDir "build-mingw-ninja\WinRing0x64.sys"),
    (Join-Path $engineDir "build-mingw-make\WinRing0x64.sys"),
    (Join-Path $repoRoot "release\unique-rx-windows-x64\WinRing0x64.sys")
)
$foundWinRing = $winRingCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($foundWinRing) {
    if (-not [string]::Equals([IO.Path]::GetFullPath($foundWinRing), [IO.Path]::GetFullPath($winRingSource), [StringComparison]::OrdinalIgnoreCase)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $winRingSource) | Out-Null
        Copy-Item -LiteralPath $foundWinRing -Destination $winRingSource -Force
    }
    Write-Host "WinRing0x64.sys locale rilevato: verrà copiato accanto al miner." -ForegroundColor Green
} else {
    Write-Warning "WinRing0x64.sys non trovato. MSR resterà opzionale e disattivato; inserisci il driver accanto a unique-rx.exe oppure in engine\msr."
}

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

# Update the portable package without deleting user files already in the output folder.
$packageDir = Join-Path $repoRoot "release\unique-rx-windows-x64"
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item -LiteralPath $exe -Destination $packageDir
Get-ChildItem -LiteralPath $exeDirectory -Filter "*.dll" -File | Copy-Item -Destination $packageDir -Force
$exeHash = (Get-FileHash -LiteralPath (Join-Path $packageDir "unique-rx.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
$shaLines = @("$exeHash  unique-rx.exe")
$winRingPackaged = $false
# The engine loads WinRing0x64.sys from beside unique-rx.exe. The build
# discovers/copies the user-supplied driver into engine\\msr, so package that
# canonical source directly instead of looking for a file CMake never stages.
$builtWinRingDriver = $winRingSource
if (Test-Path -LiteralPath $builtWinRingDriver -PathType Leaf) {
    $driverHash = (Get-FileHash -LiteralPath $builtWinRingDriver -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $builtWinRingDriver -Destination (Join-Path $packageDir "WinRing0x64.sys") -Force
    $shaLines += "$driverHash  WinRing0x64.sys"
    $winRingPackaged = $true
} else {
    Write-Host "WinRing0x64.sys non incluso: MSR opzionale; il miner CPU funziona con MSR disattivato." -ForegroundColor Yellow
}
Set-Content -LiteralPath (Join-Path $packageDir "SHA256SUMS.txt") -Encoding ascii -Value $shaLines
Copy-Item -LiteralPath (Join-Path $repoRoot "config.example.json") -Destination (Join-Path $packageDir "config.example.json")
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $repoRoot "requirements.txt") -Destination $packageDir
Copy-Item -LiteralPath (Join-Path $repoRoot "backend") -Destination $packageDir -Recurse
$dashboardOut = Join-Path $packageDir "dashboard\dist"
New-Item -ItemType Directory -Force -Path $dashboardOut | Out-Null
Copy-Item -Path (Join-Path $dashboardDir "dist\*") -Destination $dashboardOut -Recurse -Force

$licenses = Join-Path $packageDir "licenses"
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSES\XMRIG-GPL-3.0.txt") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSES\RXMINER-MIT.txt") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSES\NYXRX-MIT.txt") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSES\XMR-MINER-GPL-3.0.txt") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $engineDir "src\randomx\upstream\LICENSE") -Destination (Join-Path $licenses "RANDOMX-BSD-3-CLAUSE.txt")

$launcherDir = Join-Path $packageDir "scripts\windows"
New-Item -ItemType Directory -Force -Path $launcherDir | Out-Null
foreach ($script in @("start.bat","start.ps1","stop.bat","stop.ps1","diagnose.bat","diagnose.ps1")) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $launcherDir
}
@'
@echo off
setlocal
call "%~dp0scripts\windows\start.bat" %*
exit /b %errorlevel%
'@ | Set-Content -LiteralPath (Join-Path $packageDir "start.bat") -Encoding ascii
@'
@echo off
setlocal
call "%~dp0scripts\windows\stop.bat" %*
exit /b %errorlevel%
'@ | Set-Content -LiteralPath (Join-Path $packageDir "stop.bat") -Encoding ascii
@'
@echo off
setlocal
call "%~dp0scripts\windows\diagnose.bat" %*
exit /b %errorlevel%
'@ | Set-Content -LiteralPath (Join-Path $packageDir "diagnose.bat") -Encoding ascii

$gitStatus = & git -C $repoRoot status --porcelain
if ($LASTEXITCODE -ne 0) { throw "Impossibile verificare lo stato Git per il pacchetto sorgente." }
if ($gitStatus) { throw "Il worktree Git non è pulito: il pacchetto sorgente deve corrispondere esattamente al codice compilato." }
$sourceZip = Join-Path $packageDir "unique-rx-source.zip"
& git -C $repoRoot archive --format=zip "--output=$sourceZip" HEAD
if ($LASTEXITCODE -ne 0) { throw "Impossibile creare il sorgente corrispondente al commit compilato." }
$sourceHash = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash.ToLowerInvariant()
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Impossibile determinare il commit sorgente." }
Set-Content -LiteralPath (Join-Path $packageDir "SOURCE-COMMIT.txt") -Encoding ascii -Value $commit
$sourceSums = Join-Path $packageDir "SHA256SUMS.txt"
Add-Content -LiteralPath $sourceSums -Encoding ascii -Value "$sourceHash  unique-rx-source.zip"

Write-Host "Pacchetto completo: motore, backend, dashboard, script, licenze e sorgente corrispondente inclusi."
Write-Host ""
Write-Host "[OK] Dashboard, backend, motore nativo e suite automatica completati." -ForegroundColor Green
Write-Host "Miner: $exe"
Write-Host "Pacchetto portatile: $packageDir"
Write-Host "Dashboard: $(Join-Path $packageDir 'dashboard\dist\index.html')"
Write-Host "Log Stratum: $(Join-Path $repoRoot 'unique-rx-stratum.log')"
