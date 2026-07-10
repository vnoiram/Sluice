[CmdletBinding()]
param(
  [string]$BuildDir = "build"
)

# Runs inside the Windows Docker container (Dockerfile.engine.windows).
# The ASIO SDK is not bundled in the image, so BUILD_ASIO_HOST=OFF here: this
# only builds/runs the platform-independent core (spsc_ring / drift) tests.

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

& cmake -S $Root -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DBUILD_ASIO_HOST=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
