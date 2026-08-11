[CmdletBinding()]
param(
  [string]$BuildDir = "build"
)

# Runs inside the Windows Docker container (Dockerfile.engine.windows).
# BUILD_ASIO_HOST is left at its CMakeLists.txt default (ON). No ASIO SDK is
# required -- engine/ builds against ../asio-abi/ (an independent clean-room
# ABI implementation, see asio-abi/README.md) -- so this always builds
# sluice-engine.exe for real on Windows, not just the core tests.

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
