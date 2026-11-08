$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"
$dashboardDir = Join-Path $repoRoot "dashboard"

# The release build must never silently reuse a stale dashboard bundle.
$node = Get-Command node -ErrorAction SilentlyContinue
$npm = Get-Command npm -ErrorAction SilentlyContinue
if (-not $node -or -not $npm) {
    throw "Node.js e npm sono obbligatori per la build completa della dashboard. Installa Node.js 20.19+ o 22.12+."
}
$nodeVersionText = (& $node.Source --version).Trim().TrimStart("v")
try { $nodeVersion = [version]$nodeVersionText } catch {
    throw "Versione Node.js non riconosciuta: $nodeVersionText"
}
$nodeSupported = ($nodeVersion.Major -eq 20 -and $nodeVersion -ge [version]"20.19") -or
                 ($nodeVersion -ge [version]"22.12")
if (-not $nodeSupported) {
    throw "Node.js $nodeVersionText non supportato da Vite. Usa Node.js 20.19+ o 22.12+."
}

# Prepare the backend environment before starting the final native build.
$venvDir = Join-Path $repoRoot ".venv"
$pythonExe = Join-Path $venvDir "Scripts\python.exe"
if (-not (Test-Path -LiteralPath $pythonExe)) {
    New-Item -ItemType Directory -Path $venvDir -Force | Out-Null
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        & $launcher.Source -3 -m venv $venvDir
    } else {
        $basePython = Get-Command python -ErrorAction SilentlyContinue
        if (-not $basePython) { throw "Python 3 è obbligatorio per backend e test." }
        & $basePython.Source -m venv $venvDir
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pythonExe)) {
        throw "Creazione ambiente Python non riuscita."
    }
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
if (-not (Test-Path (Join-Path $dashboardDir "dist\index.html"))) {
    throw "Build dashboard terminata senza dashboard\dist\index.html."
}

# Prefer the already-used MSYS2 MinGW-w64 installation when present.
$msysBin = "C:\msys64\mingw64\bin"
if (Test-Path -LiteralPath $msysBin) {
    $pathEntries = $env:PATH -split [IO.Path]::PathSeparator
    if ($pathEntries -notcontains $msysBin) {
        $env:PATH = "$msysBin$([IO.Path]::PathSeparator)$env:PATH"
    }
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { throw "CMake è obbligatorio. Installa CMake e riapri PowerShell." }
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if (-not $ctest) { throw "CTest non trovato: reinstalla CMake con i componenti di testing." }

$gxx = Get-Command g++ -ErrorAction SilentlyContinue
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
$mingwMake = Get-Command mingw32-make -ErrorAction SilentlyContinue

if ($gxx) {
    $compilerBin = Split-Path -Parent $gxx.Source
    $mingwPrefix = Split-Path -Parent $compilerBin
    $opensslHeader = Join-Path $mingwPrefix "include\openssl\ssl.h"
    if (-not (Test-Path -LiteralPath $opensslHeader)) {
        throw "OpenSSL MinGW non trovato in $mingwPrefix. Installa mingw-w64-x86_64-openssl."
    }

    if ($ninja) {
        $generator = "Ninja"
        $buildDir = Join-Path $engineDir "build-mingw-ninja"
    } elseif ($mingwMake) {
        $generator = "MinGW Makefiles"
        $buildDir = Join-Path $engineDir "build-mingw-make"
    } else {
        throw "Toolchain MinGW rilevata ma manca Ninja o mingw32-make. Installa mingw-w64-x86_64-ninja oppure mingw-w64-x86_64-make."
    }

    Write-Host "Toolchain: MinGW-w64 + $generator."
    $configureArgs = @(
        "-S", $engineDir, "-B", $buildDir, "-G", $generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_C_COMPILER=$($gxx.Source -replace 'g\+\+(.exe)?$','gcc$1')",
        "-DCMAKE_CXX_COMPILER=$($gxx.Source)",
        "-DCMAKE_PREFIX_PATH=$mingwPrefix",
        "-DOPENSSL_ROOT_DIR=$mingwPrefix"
    )
    & $cmake.Source @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "Configurazione CMake MinGW fallita." }
}
else {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
    if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
        throw "Toolchain non disponibile. Installa MSYS2 MinGW-w64 GCC + OpenSSL + Ninja, oppure Visual Studio 2022 con vcpkg e configura VCPKG_INSTALLATION_ROOT."
    }
    $toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) { throw "Toolchain vcpkg non trovata: $toolchain" }
    $vcpkg = Join-Path $vcpkgRoot "vcpkg.exe"
    if (Test-Path $vcpkg) {
        & $vcpkg install openssl:x64-windows
        if ($LASTEXITCODE -ne 0) { throw "Installazione OpenSSL tramite vcpkg fallita." }
    }
    $buildDir = Join-Path $engineDir "build-msvc"
    Write-Host "Toolchain: Visual Studio 2022 + vcpkg."
    $configureArgs = @(
        "-S", $engineDir, "-B", $buildDir, "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    )
    & $cmake.Source @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "Configurazione CMake MSVC fallita." }
}

Write-Host "=== COMPILAZIONE NATIVA RELEASE ===" -ForegroundColor Cyan
& $cmake.Source --build $buildDir --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Compilazione motore/test nativi fallita." }

Write-Host "=== TEST BACKEND ===" -ForegroundColor Cyan
Push-Location $repoRoot
try {
    & $pythonExe -m unittest backend.tests.test_main
    if ($LASTEXITCODE -ne 0) { throw "Test backend falliti." }
} finally {
    Pop-Location
}

Write-Host "=== TEST NATIVI ===" -ForegroundColor Cyan
& $ctest.Source --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Uno o più test nativi sono falliti." }

$exeCandidates = @(
    (Join-Path $buildDir "unique-rx.exe"),
    (Join-Path $buildDir "Release\unique-rx.exe")
)
$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $exe) { throw "Build completata ma unique-rx.exe non trovato nella cartella di output." }

Write-Host ""
Write-Host "[OK] Dashboard, backend, motore nativo e suite automatica completati." -ForegroundColor Green
Write-Host "Miner: $exe"
Write-Host "Dashboard: $(Join-Path $dashboardDir 'dist\index.html')"
Write-Host "Log Stratum: $(Join-Path $repoRoot 'unique-rx-stratum.log')"
Write-Host "Il mining non viene avviato dalla build."
