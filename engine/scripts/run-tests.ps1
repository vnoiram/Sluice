[CmdletBinding()]
param(
  [string]$BuildDir = "build"
)

# Runs inside the Windows Docker container (Dockerfile.engine.windows).
# BUILD_ASIO_HOST is left at its CMakeLists.txt default (ON, with automatic
# fallback to OFF if engine/thirdparty/asiosdk is not present in the mounted
# source tree) so this builds sluice-engine.exe for real whenever the ASIO
# SDK has been placed by the caller, and still degrades gracefully to
# core-tests-only when it hasn't.

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
