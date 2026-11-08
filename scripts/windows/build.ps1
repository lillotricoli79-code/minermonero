$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$engineDir = Join-Path $repoRoot "engine"
$buildDir = Join-Path $engineDir "build"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake is required. Install CMake and reopen PowerShell."
}

$vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
    throw "VCPKG_INSTALLATION_ROOT is not set. Install OpenSSL with vcpkg and set the vcpkg root."
}

$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain file not found: $toolchain"
}

Write-Host "Configuring native RandomX miner..."
cmake -S $engineDir -B $buildDir -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

Write-Host "Building miner and native tests..."
cmake --build $buildDir --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

Write-Host "Running native tests..."
ctest --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

Write-Host "Build and tests completed successfully."
