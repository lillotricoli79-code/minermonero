$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"

$npm = Get-Command npm -ErrorAction SilentlyContinue
if ($npm) {
    Write-Host "Building local dashboard..."
    & $npm.Source ci --prefix (Join-Path $repoRoot "dashboard")
    if ($LASTEXITCODE -ne 0) { throw "Dashboard dependency installation failed." }
    & $npm.Source run build --prefix (Join-Path $repoRoot "dashboard")
    if ($LASTEXITCODE -ne 0) { throw "Dashboard build failed." }
}
else {
    Write-Warning "Node.js/npm not found: dashboard build skipped. Install Node.js 22+ to build the UI."
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake is required. Install CMake and reopen PowerShell."
}

$gxx = Get-Command g++ -ErrorAction SilentlyContinue
$ninja = Get-Command ninja -ErrorAction SilentlyContinue

if ($gxx -and $ninja) {
    $compilerBin = Split-Path -Parent $gxx.Source
    $mingwPrefix = Split-Path -Parent $compilerBin
    $buildDir = Join-Path $engineDir "build-mingw"

    Write-Host "Using the MinGW-w64 toolchain (no Visual Studio required)."
    $configureArgs = @(
        "-S", $engineDir, "-B", $buildDir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_C_COMPILER=gcc",
        "-DCMAKE_CXX_COMPILER=g++",
        "-DCMAKE_PREFIX_PATH=$mingwPrefix",
        "-DOPENSSL_ROOT_DIR=$mingwPrefix"
    )
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
}
else {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
    if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
        throw "Install MSYS2 MinGW-w64 GCC + Ninja + OpenSSL, or install vcpkg and set VCPKG_INSTALLATION_ROOT."
    }

    $toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) {
        throw "vcpkg toolchain file not found: $toolchain"
    }

    $vcpkg = Join-Path $vcpkgRoot "vcpkg.exe"
    if (Test-Path $vcpkg) {
        Write-Host "Ensuring OpenSSL is installed for the Visual Studio toolchain..."
        & $vcpkg install openssl:x64-windows
        if ($LASTEXITCODE -ne 0) { throw "vcpkg OpenSSL installation failed." }
    }

    $buildDir = Join-Path $engineDir "build-msvc"
    Write-Host "Using Visual Studio 2022 and vcpkg."
    $configureArgs = @(
        "-S", $engineDir, "-B", $buildDir, "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DBUILD_TESTING=ON",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    )
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
}

Write-Host "Building miner and native tests..."
& cmake --build $buildDir --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

Write-Host "Running native tests..."
& ctest --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

Write-Host "Build and tests completed successfully."
